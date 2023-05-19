#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;


  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}


#ifdef LAB_PGTBL
int
sys_pgaccess(void)
{
  // lab pgtbl: your code here.
  //先提取一下参数
  struct proc* p =myproc();
  uint64 usrpge_ptr;//待检测页表起始指针
  int npage;//待检测页表个数
  uint64 useraddr;//稍后写入用户内存
  argaddr(0,&usrpge_ptr);
  argint(1,&npage);
  argaddr(2,&useraddr);
  if(npage>64)
  {
    return -1;
  }
  uint64 bitmap=0;
  uint64 mask=1;
  uint64 complement=PTE_A;
  complement=~complement;
  int count=0;

  for(uint64 page =usrpge_ptr;page<usrpge_ptr+npage*PGSIZE;page+=PGSIZE)
  {
    pte_t* pte = walk(p->pagetable,page,0);
    if(*pte&PTE_A)
    {
      bitmap=bitmap|(mask<<count);
      *pte=(*pte)&complement;
    }
    count++;
    //printf("bitmap:%p\n",bitmap);

  }
  copyout(p->pagetable,useraddr,(char*)&bitmap,sizeof(bitmap));
  
  return 0;
}
#endif

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
