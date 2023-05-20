// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"


struct page_ref{
  struct spinlock lock;
  int num;
};

struct page_ref page_ref[(PHYSTOP - KERNBASE)/PGSIZE];


void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{

  for(int i=0;i<(PHYSTOP - KERNBASE)/PGSIZE;i++)
  {
    initlock(&(page_ref[i].lock),"page_ref");
  }

  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    page_ref[((uint64)p-KERNBASE)/PGSIZE].num=1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP){
    printf("%p,%d\n",pa,get_page_ref((void*)pa));     
    panic("kfree");
  }

  uint64 i=((uint64)pa-KERNBASE)/PGSIZE;
  acquire(&page_ref[i].lock);
  if(page_ref[i].num==0)
  {
    release(&page_ref[i].lock);
    panic("kfree: too much times");
  }
  page_ref[i].num--;
  if(page_ref[i].num!=0)
  {
    release(&page_ref[i].lock);
    return;
  }
  else
  {
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run*)pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);

    release(&page_ref[i].lock);    
  }
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    uint64 i=((uint64)r-KERNBASE)/PGSIZE;
    acquire(&page_ref[i].lock);
    page_ref[i].num=1;
    release(&page_ref[i].lock);
    kmem.freelist = r->next;
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void add_page_ref(void* pa)
{
  uint64 i=((uint64)pa-KERNBASE)/PGSIZE;
  acquire(&page_ref[i].lock);
  page_ref[i].num++;
  release(&page_ref[i].lock);
}

void dec_page_ref(void* pa)
{
  uint64 i=((uint64)pa-KERNBASE)/PGSIZE;
  acquire(&page_ref[i].lock);
  page_ref[i].num--;
  release(&page_ref[i].lock);
}

//这个加的锁实际上没有作用。
int get_page_ref(void* pa)
{
  uint64 i=((uint64)pa-KERNBASE)/PGSIZE;
  acquire(&page_ref[i].lock);
  int num=page_ref[((uint64)pa-KERNBASE)/PGSIZE].num;
  release(&page_ref[i].lock);
  return num;
}
