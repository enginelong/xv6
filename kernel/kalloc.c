// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU]; // 【每个CPU分配一个kmem锁】

#define MAX_NUM_PAGES 100	// 【最大“偷窃”页数设为100】

void * get_others_pages(int cpu){
  int count = 0;
  struct run *start = 0;
  struct run *end = 0;
  for(int i = 0; i < NCPU; i++){
    if(i == cpu)
      continue;
    
    acquire(&kmem[i].lock);
    
    start = kmem[i].freelist;
    end = kmem[i].freelist;
    if(!start){	// 这个CPU也没有空闲内存
      release(&kmem[i].lock);
      continue;
    }
   	// 链表向后，直到结束或者达到100页
    while(end && count < MAX_NUM_PAGES){
      end = end->next;
      count++;
    }
    if(end){ 
      kmem[i].freelist = end->next;	// 后面还有空闲内存，freelist接在后面
      end->next = 0;
    }
    else kmem[i].freelist = 0;
    release(&kmem[i].lock);

    acquire(&kmem[cpu].lock);
    kmem[cpu].freelist = start->next;	// cpu的空闲内存freelist从start开始
    release(&kmem[cpu].lock);
    break;
    
  }
  return (void*) start;
}

void
kinit()
{
  char name[10];
  for (int i = 0; i < NCPU; i++) {
    snprintf(name, 10, "kmem-%d", i);
    initlock(&kmem[i].lock, name);
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int cpu = cpuid();
  pop_off();
  // 空闲页加入对应CPU的空闲页链表
  acquire(&kmem[cpu].lock);
  r->next = kmem[cpu].freelist;
  kmem[cpu].freelist = r;	// 维护链表（从头部插入）
  release(&kmem[cpu].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int cpu = cpuid();
  pop_off();

  acquire(&kmem[cpu].lock);
  r = kmem[cpu].freelist;

  if(r)
    kmem[cpu].freelist = r->next;
  release(&kmem[cpu].lock);
  if(r == 0)
    r = get_others_pages(cpu);  // 【如果当前CPU的空闲页链表为空，到其他CPU的空闲页中取】

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
