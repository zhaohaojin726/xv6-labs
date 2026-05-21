# xv6 文件读取流程详解

本文按 xv6 当前源码解释一次普通文件读取的完整路径：

```text
user read()
  -> sys_read
  -> fileread
  -> readi
  -> bmap
  -> bread
  -> bget
  -> virtio_disk_rw
  -> either_copyout/copyout
  -> 返回用户态
```

相关文件：

```text
kernel/sysfile.c       系统调用参数解析
kernel/file.c          struct file 层
kernel/fs.c            inode 和文件系统层
kernel/bio.c           buffer cache 层
kernel/buf.h           struct buf
kernel/virtio_disk.c   磁盘驱动层
kernel/proc.c          either_copyout
kernel/vm.c            copyout
```

## 一、总体分层

xv6 文件读取不是直接从用户态访问磁盘，而是经过多层抽象：

```text
用户程序
  |
  v
系统调用层
  |
  v
文件描述符层 struct file
  |
  v
inode 层 struct inode
  |
  v
block 映射 bmap
  |
  v
buffer cache
  |
  v
virtio 磁盘驱动
  |
  v
磁盘
```

用户看到的是：

```c
read(fd, buf, n);
```

内核真正做的是：

```text
根据 fd 找到 struct file
根据 struct file 找到 inode 和当前 offset
根据 inode 和 offset 找到磁盘 blockno
通过 buffer cache 读取磁盘块
把数据从内核 buffer 拷贝到用户 buffer
更新文件 offset
返回读取字节数
```

## 二、用户态 read

用户程序调用：

```c
read(fd, buf, n);
```

参数含义：

```text
fd：
  文件描述符，是当前进程打开文件表的下标。

buf：
  用户空间缓冲区地址，读取到的数据要放到这里。

n：
  最多读取多少字节。
```

进入内核前，RISC-V 调用约定中：

```text
a0 = fd
a1 = buf
a2 = n
a7 = SYS_read
```

然后执行 `ecall` 进入 xv6 内核。

## 三、sys_read：取系统调用参数

位置：`kernel/sysfile.c`

```c
uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}
```

这里做三件事：

```text
argfd(0, 0, &f)：
  从第 0 个系统调用参数取 fd。
  根据 fd 找到当前进程的 struct file *。

argaddr(1, &p)：
  从第 1 个系统调用参数取用户缓冲区地址。

argint(2, &n)：
  从第 2 个系统调用参数取读取长度。
```

然后调用：

```c
fileread(f, p, n);
```

注意：

```text
p 是用户虚拟地址。
内核不能直接通过 *(char*)p 写用户内存。
后面必须通过 copyout。
```

## 四、fileread：根据 file 类型分发

位置：`kernel/file.c`

```c
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else {
    panic("fileread");
  }

  return r;
}
```

`struct file` 可能代表：

```text
FD_PIPE：
  管道。

FD_DEVICE：
  设备，例如 console。

FD_INODE：
  普通文件或目录，对应一个 inode。
```

普通文件读取走：

```c
ilock(f->ip);
readi(f->ip, 1, addr, f->off, n);
f->off += r;
iunlock(f->ip);
```

关键点：

```text
f->ip：
  文件对应的 inode。

f->off：
  当前文件偏移。
  read 从这个偏移开始读。

ilock(f->ip)：
  锁住 inode，防止文件大小、block 映射等元数据被并发修改。

user_dst = 1：
  传给 readi，表示目标地址 addr 是用户虚拟地址。
```

## 五、inode 是什么

`inode` 是文件系统中描述一个文件的核心结构。

它包含：

```text
文件类型
文件大小
链接数
磁盘 block 地址
设备号
inode 编号
```

对于读取来说，最重要的是：

```text
ip->size：
  文件大小。

ip->addrs[]：
  文件数据块在磁盘上的 block 地址。
```

文件内容不是直接存储在 inode 结构体里，而是存储在磁盘 block 中。inode 只保存这些 block 的地址。

## 六、readi：从 inode 读取数据

位置：`kernel/fs.c`

```c
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return 0;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  return tot;
}
```

`readi()` 是普通文件读取的核心。

参数：

```text
ip：
  要读取的 inode。

user_dst：
  目标地址是否是用户地址。
  read 系统调用传入 1。

dst：
  目标缓冲区地址。
  对 read 来说，这是用户虚拟地址。

off：
  文件偏移。

n：
  最多读取多少字节。
```

### 1. 检查读取范围

```c
if(off > ip->size || off + n < off)
  return 0;
```

含义：

```text
如果 offset 已经超过文件大小，读不到数据。
如果 off + n 溢出，也返回 0。
```

然后：

```c
if(off + n > ip->size)
  n = ip->size - off;
```

如果用户要求读的字节数超过文件末尾，就截断到文件大小。

例如：

```text
文件大小 100 字节
off = 80
n = 50

实际只能读 20 字节
```

### 2. 按块循环读取

```c
for(tot=0; tot<n; tot+=m, off+=m, dst+=m)
```

文件系统以 block 为单位管理磁盘数据。

xv6 中：

```text
BSIZE = 1024
```

所以一次 read 可能跨多个磁盘块。

循环每次处理当前 offset 所在的一个 block 的一部分。

### 3. 找到当前文件偏移对应的磁盘块

```c
bmap(ip, off/BSIZE)
```

`off/BSIZE` 是文件内的第几个 block。

例如：

```text
off = 0       -> 文件第 0 个 block
off = 1024    -> 文件第 1 个 block
off = 2048    -> 文件第 2 个 block
off = 1500    -> 文件第 1 个 block，块内偏移 476
```

`bmap()` 会把“文件内第几个 block”转换成“磁盘上的真实 blockno”。

## 七、bmap：文件块号到磁盘块号

位置：`kernel/fs.c`

```c
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}
```

`bmap()` 处理两类块：

```text
直接块：
  ip->addrs[0] 到 ip->addrs[NDIRECT-1]

间接块：
  ip->addrs[NDIRECT] 指向一个“间接块”
  间接块中保存更多数据块号
```

对于读取已有文件，通常这些地址已经存在。

注意：

```text
bmap() 如果发现对应块号为 0，会分配新块。
这对 write 很重要。
对正常 read 来说，因为 off 被限制在 ip->size 内，通常应该能找到已有块。
```

## 八、bread：读取一个磁盘 block 到 buffer cache

位置：`kernel/bio.c`

```c
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
```

`bread()` 的目标是：

```text
返回一个已经锁住的 struct buf。
这个 buf->data 中包含 dev/blockno 对应磁盘块的数据。
```

流程：

```text
1. bget(dev, blockno) 从 buffer cache 找或分配一个 buf。
2. 如果 b->valid == 1，说明缓存中已有有效数据，直接返回。
3. 如果 b->valid == 0，调用 virtio_disk_rw(b, 0) 从磁盘读入。
4. 读完后设置 b->valid = 1。
```

## 九、bget：buffer cache 查找或复用

位置：`kernel/bio.c`

```c
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);

  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}
```

`bget()` 有两个阶段。

### 1. 查找缓存命中

```c
for(b = bcache.head.next; b != &bcache.head; b = b->next){
  if(b->dev == dev && b->blockno == blockno){
    b->refcnt++;
    release(&bcache.lock);
    acquiresleep(&b->lock);
    return b;
  }
}
```

如果 cache 中已经有这个块：

```text
refcnt++
释放 bcache.lock
获取该 buf 的 sleeplock
返回 buf
```

为什么先增加 `refcnt`？

```text
表示这个 buffer 正在被使用，不能被别人复用。
```

为什么还要 `acquiresleep(&b->lock)`？

```text
bcache.lock 只保护 cache 链表和 refcnt。
b->lock 保护这个 buffer 的具体 data 内容。
同一时间只有一个进程可以读写某个 buf->data。
```

### 2. 缓存未命中，找空闲 buffer 复用

```c
for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
  if(b->refcnt == 0) {
    b->dev = dev;
    b->blockno = blockno;
    b->valid = 0;
    b->refcnt = 1;
    release(&bcache.lock);
    acquiresleep(&b->lock);
    return b;
  }
}
```

如果没有命中，就找一个没有人使用的 buffer：

```text
refcnt == 0
```

复用它来代表新的磁盘块：

```text
设置 dev/blockno
valid = 0，表示还没从磁盘读入数据
refcnt = 1
```

然后返回这个 buf。

## 十、struct buf：磁盘块缓存对象

位置：`kernel/buf.h`

```c
struct buf {
  int valid;
  int disk;
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev;
  struct buf *next;
  uchar data[BSIZE];
};
```

字段含义：

```text
valid：
  data 中是否有有效磁盘数据。

disk：
  磁盘驱动是否正在使用这个 buf。

dev：
  设备号。

blockno：
  磁盘块号。

lock：
  sleeplock，保护 data。

refcnt：
  当前有多少引用者正在使用它。

prev/next：
  在 bcache LRU 链表中的位置。

data：
  真实缓存的磁盘块内容。
```

## 十一、virtio_disk_rw：真正读磁盘

位置：`kernel/virtio_disk.c`

```c
void
virtio_disk_rw(struct buf *b, int write)
```

对于读取：

```c
virtio_disk_rw(b, 0);
```

其中：

```text
write = 0 表示从磁盘读入。
```

核心片段：

```c
uint64 sector = b->blockno * (BSIZE / 512);
```

xv6 文件系统 block 大小是 `BSIZE`，virtio 磁盘 sector 是 512 字节，所以要换算。

读请求中：

```c
buf0->type = VIRTIO_BLK_T_IN;
```

表示：

```text
从磁盘读到内存。
```

数据 descriptor：

```c
disk.desc[idx[1]].addr = (uint64) b->data;
disk.desc[idx[1]].len = BSIZE;
disk.desc[idx[1]].flags = VRING_DESC_F_WRITE;
```

含义：

```text
设备可以把读取到的数据写入 b->data。
```

然后通知设备：

```c
*R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;
```

并睡眠等待磁盘中断完成：

```c
while(b->disk == 1) {
  sleep(b, &disk.vdisk_lock);
}
```

磁盘中断到来后，`virtio_disk_intr()` 会唤醒等待者。

## 十二、把数据复制到用户空间

回到 `readi()`：

```c
if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
  brelse(bp);
  tot = -1;
  break;
}
```

`bp->data + (off % BSIZE)` 是当前块内要读取的位置。

例如：

```text
BSIZE = 1024
off = 1500

off / BSIZE = 1
off % BSIZE = 476

读取文件第 1 个 block 中偏移 476 开始的数据。
```

`m` 是本轮要复制的字节数：

```c
m = min(n - tot, BSIZE - off%BSIZE);
```

表示：

```text
不能超过用户还需要的字节数。
不能超过当前磁盘块剩余空间。
```

## 十三、either_copyout

位置：`kernel/proc.c`

```c
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}
```

因为 `readi()` 不只服务于用户态 read，也可能给内核地址拷贝数据，所以它用 `user_dst` 区分：

```text
user_dst = 1：
  dst 是用户虚拟地址。
  必须通过 copyout。

user_dst = 0：
  dst 是内核地址。
  可以直接 memmove。
```

对于系统调用 `read()`：

```text
user_dst = 1
```

所以最终走：

```c
copyout(p->pagetable, dst, src, len);
```

`copyout()` 会通过当前进程页表，把用户虚拟地址转换到物理地址，并把内核数据复制过去。

## 十四、brelse：释放 buffer

每次 `readi()` 复制完当前 block 的数据后：

```c
brelse(bp);
```

`brelse()` 做的是：

```text
释放 bp->lock
refcnt--
如果 refcnt == 0，把这个 buffer 移到 LRU 头部
```

原始实现：

```c
releasesleep(&b->lock);

acquire(&bcache.lock);
b->refcnt--;
if (b->refcnt == 0) {
  b->next->prev = b->prev;
  b->prev->next = b->next;
  b->next = bcache.head.next;
  b->prev = &bcache.head;
  bcache.head.next->prev = b;
  bcache.head.next = b;
}
release(&bcache.lock);
```

含义：

```text
这个 buffer 当前使用结束。
如果没人再引用它，它可以成为可复用 buffer。
LRU 链表会记录最近使用情况。
```

## 十五、fileread 更新文件 offset

`readi()` 返回后，回到 `fileread()`：

```c
if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
  f->off += r;
```

如果成功读到 `r` 字节，就更新文件偏移：

```text
下一次 read 从新的 f->off 开始。
```

例如：

```text
第一次 read 读取 100 字节，f->off 从 0 变成 100。
第二次 read 会从文件偏移 100 继续读。
```

## 十六、锁的关系

文件读取路径里有多种锁。

### 1. inode sleeplock

```c
ilock(f->ip);
...
iunlock(f->ip);
```

保护 inode 元数据：

```text
文件大小
block 映射
inode 内容
```

### 2. bcache.lock

```c
acquire(&bcache.lock);
```

保护 buffer cache 的全局链表和 `refcnt`。

lock 实验中，这就是要优化的热点锁。

### 3. buf sleeplock

```c
acquiresleep(&b->lock);
```

保护某个具体磁盘块 buffer 的 `data`。

### 4. virtio disk lock

```c
acquire(&disk.vdisk_lock);
```

保护 virtio 磁盘队列。

## 十七、完整例子

假设用户执行：

```c
char buf[1500];
read(fd, buf, 1500);
```

并且当前文件偏移：

```text
f->off = 500
```

流程：

```text
sys_read 取得 fd、buf、1500
fileread 找到 struct file
fileread 锁住 inode
readi 从 off=500 开始读
```

第一轮：

```text
off = 500
off / BSIZE = 0
off % BSIZE = 500

读取文件第 0 个 block 的偏移 500 开始
最多读 BSIZE - 500 = 524 字节

bmap 找到磁盘 blockno
bread 读取该 block
copyout 524 字节到用户 buf[0..523]
brelse
```

第二轮：

```text
off = 1024
读取文件第 1 个 block
最多读 1024 字节
但用户还需要 1500 - 524 = 976 字节

copyout 976 字节到用户 buf[524..1499]
brelse
```

最终：

```text
readi 返回 1500
fileread 更新 f->off += 1500
sys_read 返回 1500
用户 read() 返回 1500
```

## 十八、和 lock 实验的关系

在当前 lock 实验中，文件读取路径最重要的是：

```text
readi -> bread -> bget
```

因为 `bget()` 当前会抢全局：

```c
bcache.lock
```

多个进程并发读不同文件时，都会进入：

```text
bread
  -> bget
      -> acquire(&bcache.lock)
```

于是产生大量锁竞争。

lock 实验要求优化这一点：

```text
把全局 bcache.lock 拆成多个 bucket lock。
按 blockno hash 到不同 bucket。
不同 block 尽量走不同锁。
```

但无论怎么优化，都必须保持：

```text
同一个 dev/blockno 在缓存中最多只有一个 struct buf。
refcnt 的修改必须受对应锁保护。
b->data 必须由 b->lock 保护。
```

## 十九、总结

文件读取的核心链路：

```text
read(fd, buf, n)
  -> sys_read
      解析 fd、buf、n
  -> fileread
      根据 fd 找到 struct file
      对普通文件锁 inode
      调用 readi
  -> readi
      按文件 offset 和 n 分块读取
      用 bmap 找磁盘 blockno
      用 bread 取 block 内容
      用 either_copyout/copyout 拷贝到用户空间
      brelse 释放 buffer
  -> fileread
      更新 f->off
  -> 返回用户态
```

一句话：

```text
read 不是直接读磁盘；
它是通过 inode 找到文件块，通过 bcache 找到或读取磁盘块，再把 buffer cache 中的数据 copyout 到用户空间。
```
