#include "vma.h"
#include "param.h"
#include "defs.h"
#include "fcntl.h"
#include "file.h"
#include "proc.h"

// Free VMA memory, set all vma data in the vma list to 0 and decrement references to (and close) files 
void
vma_free(struct proc * p) {
    struct vma * vma_list = &(p->vma_list);
    for (int i = 0; i < MAXVMA; i++)
    {
        vma_free_pages(vma_list, i, vma_list->addr[i],  vma_list->length[i], p->pagetable);
        vma_list->addr[i] = 0;
        vma_list->length[i] = 0;
        vma_list->offset[i] = 0;
        vma_list->prot[i] = 0;
        vma_list->flags[i] = 0;
        vma_list->prot[i] = 0;
        vma_list->fd[i] = 0;
        if (vma_list->file[i] != 0)
            fileclose(vma_list->file[i]);
        vma_list->file[i] = 0;
        vma_list->bottom_addr = INITIAL_BOTTOM_ADDR;
    }
}

// Return the index in the vma list of the vma where addr is pointing into or return -1 if
// addr is not pointing into any vma.
int
vma_find(struct vma * vma_list, uint64 addr) {
    for (int i = 0; i < MAXVMA; i++)
    {
        uint64 vma_addr = vma_list->addr[i];
        uint64 vma_length = vma_list->length[i];
        if ((vma_addr <= addr) && (addr < vma_addr + vma_length))
            return i;
    }
    return -1;
}

//Return the index in the vma list of a free vma entry or return -1 if there are no free entries left
int
vma_find_free_entry(struct vma * vma_list) {
    for (int i = 0; i < MAXVMA; i++)
    {
        if (! vma_list->length[i]) return i;
    }
    return -1;
}

//Fill "index"-th entry of vma_list with given parameters. Returns -1 if index is not valid, and 0 otherwise.
int
vma_fill(struct vma * vma_list, int index, uint64 addr, int offset, int length, int prot, int flags, int fd, struct file * f)
{
    if ((index < 0) || (index >= MAXVMA))
        return -1;
    vma_list->addr[index] = addr;
    vma_list->offset[index] = offset;
    vma_list->length[index] = length;
    vma_list->prot[index] = prot;
    vma_list->flags[index] = flags;
    vma_list->fd[index] = fd;
    vma_list->file[index] = f;
    return 0;
}

//Allocate space for new vma given its length and return virtual address.
//Length does not need to be page-aligned.
uint64
vma_get_new_addr(struct vma * vma_list, uint64 length)
{
    uint64 new_addr = PGROUNDDOWN(vma_list->bottom_addr - length);
    vma_list->bottom_addr = new_addr -1;
    return new_addr;
}

//Returns size of freed region or -1 if parameters were invalid.
//Expects page-aligned address pointing to the same VMA referenced by index.
int
vma_free_pages(struct vma * vma_list, int index, uint64 page_addr, int length, pagetable_t pagetable)
{
    uint64 first_vma_addr = vma_list->addr[index];
    int vma_length = vma_list->length[index];
    uint64 last_vma_addr = first_vma_addr + vma_length -1;
    //Check whether page_addr belongs to vma and is page-aligned
    if ((page_addr < first_vma_addr) || (last_vma_addr < page_addr) || ((page_addr % PGSIZE) != 0))
        return -1;
    // Round up length of the section to free to the nearest page size.
    int free_length = PGROUNDUP(length);
    // Length of the section to free is either free_length or the length from page_addr to the end of the vma (if the former exceeds the end of the vma)
    free_length = (page_addr + free_length > last_vma_addr ? last_vma_addr+1 - page_addr : free_length);

    int freed = 0;
    for (; freed < free_length; freed += PGSIZE)
    {
        uint64 physical_addr = walkaddr(pagetable, page_addr + freed);
        //If not page is not mapped we can skip it.
        if(physical_addr == 0)
            continue;
        //Check if mapping is shared
        if (vma_list->flags[index] == MAP_SHARED)
        {
            // If page is dirty (has been written), write it back to file.
            pte_t * pte = walk(pagetable, page_addr + freed, 0);
            if(*pte & PTE_D)
            {
                struct file * f = vma_list->file[index];
                begin_op();
                ilock(f->ip);
                writei(f->ip, 1, page_addr + freed, vma_list->offset[index] + freed, PGSIZE);
                iunlock(f->ip);
                end_op();
            }
        }
        // Our references mechanism is only used when working with pages of VMAs
        // so we won't bother using it with other pages (in uvmunmap instead of here for example).
        // (That said, uvmcopypages() is defined at vm.c and uses this mechanism...
        //  But note that said function was introduced by us and we have only used it to
        //  copy VMA pages in vma_copy() )

        // Check references to physical addr.
        int refs = getref((void *)physical_addr);
        if (refs > 1)
            decref((void *) physical_addr);
        // If only one reference left (our process'), we can free physical memory
        uvmunmap(pagetable, page_addr + freed, 1, (refs == 1));
    }

    return freed;
}

// Copies src proc VMA list to dst proc VMA list. Returns 0 on success and -1 on failure
int
vma_copy(struct proc * src, struct proc * dst){
    struct vma * src_vma_list = &(src->vma_list);
    struct vma * dst_vma_list = &(dst->vma_list);
    for (int i = 0; i < MAXVMA; i++)
    {
        //If VMA is empty we can skip copying it.
        if (src_vma_list->length[i] <= 0) continue;

        // We want src and dst to share their VMA physical pages.
        // If the VMA is MAP_SHARED there is not much to do.
        // If the VMA is MAP_PRIVATE then we have to take some additional measures:
        //      When the VMA does not have PROT_WRITE there is no problem.
        //      When it does have PROT_WRITE, we need to remove write permissions of the VMA pages
        //      so that the correspondent write exception will take place at usertrap.
        //      There we will further handle such events by making a copy of the written pages.
        // TODO: Pregunta ¿Por qué no comprobar si el mapeo tiene PROT_WRITE?
        // Si no lo tiene nos podemos ahorrar quitar permisos de escritura de las páginas porque
        // no deberían de estar presentes en primer lugar.
        if ((src_vma_list->flags[i] == MAP_PRIVATE) && (src_vma_list->prot[i] & PROT_WRITE))
        {
            // Remove write permissions to every existing PTE of the VMA
            for (uint64 page_va = src_vma_list->addr[i]; i < src_vma_list->addr[i] + src_vma_list->length[i]; i+=PGSIZE)
            {
                // Filter out empty PTEs
                if (walkaddr(src->pagetable, page_va))
                    uvmunsetflags(src->pagetable, page_va, PTE_W);
            }
        }

        // Copy PTEs associated to given interval of virtual pages from src proc to dst proc.
        if (uvmcopypages(src->pagetable, dst->pagetable, src_vma_list->addr[i], src_vma_list->length[i]) < 0)
            return -1;

        dst_vma_list->addr[i] = src_vma_list->addr[i];
        dst_vma_list->length[i] = src_vma_list->length[i];
        dst_vma_list->prot[i] = src_vma_list->prot[i];
        dst_vma_list->flags[i] = src_vma_list->flags[i];
        dst_vma_list->fd[i] = src_vma_list->fd[i];
        dst_vma_list->file[i] = src_vma_list->file[i];
        if (dst_vma_list->file[i] != 0)
            filedup(dst_vma_list->file[i]);
        dst_vma_list->bottom_addr = src_vma_list->bottom_addr;

    }
    return 0;
}