// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13
#define BUCKET_HASH(dev, blockno) (blockno % NBUCKET)

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf bucket[NBUCKET];
  struct spinlock bucket_locks[NBUCKET];
} bcache;

void
binit(void)
{
  initlock(&bcache.lock, "bcache_lock");

// 【初始化每个bucket】
  char name[20];
  for(int i = 0; i < NBUCKET; i++) {
    snprintf(name, 20, "bucket_lock_%d", i);
    initlock(&bcache.bucket_locks[i], name);
    bcache.bucket[i].next = 0;
  }
//【初始化每个buffer】
  for(int i = 0; i < NBUF; i++){
    struct buf *b = &bcache.buf[i];
    initsleeplock(&b->lock, "buffer");
    b->lastuse_time = 0;
    b->refcnt = 0;
    // 【初始：将所有buffer装到bucket[0]】
    b->next = bcache.bucket[0].next;
    bcache.bucket[0].next = b;
    b->owner = 0;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{

  uint index = BUCKET_HASH(dev, blockno);	// 【找到归属于哪个bucket管理】

  acquire(&bcache.bucket_locks[index]); // 获取对应bucket的锁

  // Is the block already cached?
  // 【在对应的bucket里面找，是否已经缓存了这个块】
  struct buf *b = bcache.bucket[index].next;
  while(b){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bucket_locks[index]);
      acquiresleep(&b->lock);
      return b;
    }
    b = b->next;
  }

  // Not cached.没有被缓存
  // 我们需要从所有buckets中, 用最近最久未使用策略(LRU), 寻找合适的buffer来缓存这个块,
  // 这表明我们需要获取它们对应的bucket_lock.
  
  // 【占有一个bucket_lock时, 尝试再获取其他bucket锁是很不安全的, 很容易循环等待造成死锁, 
  // 所以我们首先释放当前获取的bucket锁】
  release(&bcache.bucket_locks[index]);

  // 【获取大锁lock】
  acquire(&bcache.lock);

  // 【获取大锁后, 再检查一遍是否已缓存(可能有其他进程所做的操作)
  // 防止出现同一个块缓存在多个buffer的问题】
  b = bcache.bucket[index].next;
  while(b){
    if(b->dev == dev && b->blockno == blockno){
      acquire(&bcache.bucket_locks[index]);
      b->refcnt++;
      release(&bcache.bucket_locks[index]);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
    b = b->next;
  }

  // Not Cached.仍然没有被缓存
  // 【此时需要采用LRU机制选择对应的buffer】:
  struct buf *before_ans = 0;
  uint min_lastuse_time = 0xffffffff;
  uint owner = -1;
  for(int i = 0; i < NBUCKET; i++){
    acquire(&bcache.bucket_locks[i]); // 获取bucket_lock[i]
    int flag = 0; // 标记
    b = &bcache.bucket[i];
    while(b->next) {
      if(b->next->refcnt == 0 && before_ans == 0){	
      //【还没找到一个buffer(before_ans==0), 此时碰到一个空闲buffer, 暂时作为before_ans】
        before_ans = b;
        min_lastuse_time = b->next->lastuse_time;
        flag = 1;
      }
      else if(b->next->refcnt == 0 && b->next->lastuse_time < min_lastuse_time) {
      //【已经找到buffer, 但是碰到空闲buffer, 满足lastuse_time更小, 应该用这个】
        before_ans = b;
        min_lastuse_time = b->next->lastuse_time;
        flag = 1;
      }
      b = b->next;
    }
    if(flag) {  // 【bucket[i]找到新的buffer】
      if(owner != -1)	// 有暂时的owner 
        release(&bcache.bucket_locks[owner]);	// 【释放之前的owner bucket的锁】
        //【要注意一直持有的是目前最新的owner的锁！！！】
      owner = i;	// 设置新的owner
    } else {  // 【bucket[i]没有找到新的buffer: 释放buck_lock[i] (记得在循环一开始先获得了锁) 】
      release(&bcache.bucket_locks[i]);
    }
  }
  if(before_ans == 0) {	// 最终都没有找到一个buffer
    panic("bget: No buffer.");
  }

  struct buf *ans = before_ans->next;
  
  if(owner != index) { // 【找到的buffer不在当前的bucket[index]】
    // buffer从原来的bucket[owner]移除:
    before_ans->next = ans->next;
    release(&bcache.bucket_locks[owner]);

    // buffer添加到bucket[index]:
    acquire(&bcache.bucket_locks[index]);
    ans->next = bcache.bucket[index].next;
    bcache.bucket[index].next = ans;
  }

  // buffer重置:
  ans->dev = dev;
  ans->blockno = blockno;
  ans->refcnt = 1;
  ans->valid = 0;
  ans->owner = index;

  release(&bcache.bucket_locks[index]);	// 释放bucket[index]锁
  //【无论owner等不等于index，执行到此处都只持有bucket[index]的锁】
  release(&bcache.lock);	// 释放大锁
  acquiresleep(&ans->lock);
  return ans;
}


// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
// 【必须持有大锁lock】
  if(!holdingsleep(&b->lock))
    panic("brelse");
// 唤醒 buffer lock
  releasesleep(&b->lock);

  uint index = BUCKET_HASH(b->dev, b->blockno);	// 找到block归属于哪个bucket
  acquire(&bcache.bucket_locks[index]);
  b->refcnt--;
  if (b->refcnt == 0) {	//没有进程引用这块buffer, 则为空闲状态, lastuse_time设为当前时钟
    b->lastuse_time = ticks;
  }
  release(&bcache.bucket_locks[index]);
}

void
bpin(struct buf *b) {
  
  uint index = BUCKET_HASH(b->dev, b->blockno);
  acquire(&bcache.bucket_locks[index]);
  b->refcnt++;
  release(&bcache.bucket_locks[index]);
}

void
bunpin(struct buf *b) {

  uint index = BUCKET_HASH(b->dev, b->blockno);
  acquire(&bcache.bucket_locks[index]);
  b->refcnt--;
  release(&bcache.bucket_locks[index]);
}


