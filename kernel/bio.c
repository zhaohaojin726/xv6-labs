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

struct {
  struct spinlock lock[NBUCKET];
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head[NBUCKET];

  struct spinlock steal_lock;
} bcache;

static int
bhash(uint blockno)
{
  return blockno % NBUCKET;
}

static void
bremove(struct buf *b)
{
  b->next->prev = b->prev;
  b->prev->next = b->next;
}

static void
binsert(int i, struct buf *b)
{
  b->next = bcache.head[i].next;
  b->prev = &bcache.head[i];
  bcache.head[i].next->prev = b;
  bcache.head[i].next = b;
}

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.steal_lock, "steal");
  for(int i=0; i<NBUCKET; ++i){
    initlock(&bcache.lock[i], "bcache");

    // Create linked list of buffers
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
  }
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    int i = (b - bcache.buf) % NBUCKET;
    initsleeplock(&b->lock, "buffer");
    binsert(i, b);
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int i = bhash(blockno);
  acquire(&bcache.lock[i]);

  // Is the block already cached?
  for(b = bcache.head[i].next; b != &bcache.head[i]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock[i]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached. Try an unused buffer in the target bucket first.
  for(b = bcache.head[i].prev; b != &bcache.head[i]; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock[i]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  release(&bcache.lock[i]);

  acquire(&bcache.steal_lock);

  // Another CPU may have inserted the block before we got steal_lock.
  acquire(&bcache.lock[i]);
  for(b = bcache.head[i].next; b != &bcache.head[i]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock[i]);
      release(&bcache.steal_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  for(b = bcache.head[i].prev; b != &bcache.head[i]; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock[i]);
      release(&bcache.steal_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.lock[i]);

  // Steal an unused buffer from another bucket.
  for(int j=1; j<NBUCKET; ++j){
    int k = (i+j) % NBUCKET;
    acquire(&bcache.lock[k]);
    for(b = bcache.head[k].prev; b != &bcache.head[k]; b = b->prev){
      if(b->refcnt == 0) {
        bremove(b);
        release(&bcache.lock[k]);

        acquire(&bcache.lock[i]);
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        binsert(i, b);
        release(&bcache.lock[i]);
        release(&bcache.steal_lock);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bcache.lock[k]);
  }
  release(&bcache.steal_lock);
  panic("bget: no buffers");
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
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int i = bhash(b->blockno);
  acquire(&bcache.lock[i]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    bremove(b);
    binsert(i, b);
  }
  
  release(&bcache.lock[i]);
}

void
bpin(struct buf *b) {
  int i = bhash(b->blockno);
  acquire(&bcache.lock[i]);
  b->refcnt++;
  release(&bcache.lock[i]);
}

void
bunpin(struct buf *b) {
  int i = bhash(b->blockno);
  acquire(&bcache.lock[i]);
  b->refcnt--;
  release(&bcache.lock[i]);
}

