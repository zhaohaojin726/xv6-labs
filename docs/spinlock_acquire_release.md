# xv6 spinlock：acquire 和 release 实现原理

本文解释 xv6 中 `acquire()` 和 `release()` 的实现原理，重点包括：

```text
spinlock 结构体
acquire 如何抢锁
release 如何释放锁
为什么要关中断
为什么需要原子指令
为什么需要内存屏障
push_off/pop_off 的作用
```

相关文件：

```text
kernel/spinlock.h
kernel/spinlock.c
kernel/proc.h
kernel/proc.c
kernel/riscv.h
```

## 一、spinlock 解决什么问题

xv6 是多 CPU 系统。多个 CPU 可能同时运行内核代码，并访问同一份共享数据。

例如：

```text
多个 CPU 同时访问进程表
多个 CPU 同时访问物理页 freelist
多个 CPU 同时访问文件表
多个 CPU 同时访问 E1000 TX ring
```

如果不加锁，就可能出现竞态。

例如两个 CPU 同时从 freelist 分配物理页：

```text
CPU0 看到 freelist 指向 page A
CPU1 也看到 freelist 指向 page A
CPU0 返回 page A
CPU1 也返回 page A
```

结果同一物理页被分配给两个使用者，内核状态损坏。

spinlock 的目标是：

```text
同一时间只允许一个 CPU 进入临界区。
其他 CPU 如果也想进入，就原地自旋等待。
```

## 二、spinlock 结构体

定义在 `kernel/spinlock.h`：

```c
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
};
```

核心字段是：

```text
locked
```

含义：

```text
locked = 0：
  锁空闲，可以获取。

locked = 1：
  锁已经被某个 CPU 持有。
```

调试字段：

```text
name：
  锁的名字，方便 panic 时定位。

cpu：
  当前持有这把锁的 CPU。
  用于 holding() 检查。
```

## 三、initlock

定义在 `kernel/spinlock.c`：

```c
void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
}
```

初始化后：

```text
锁名 = name
locked = 0，表示未加锁
cpu = 0，表示没有 CPU 持有
```

## 四、acquire 的整体流程

代码：

```c
void
acquire(struct spinlock *lk)
{
  push_off(); // disable interrupts to avoid deadlock.
  if(holding(lk))
    panic("acquire");

  while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ;

  __sync_synchronize();

  lk->cpu = mycpu();
}
```

整体流程：

```text
1. 关闭当前 CPU 中断。
2. 检查当前 CPU 是否已经持有这把锁。
3. 使用原子 test-and-set 抢锁。
4. 如果锁已被别人持有，就自旋等待。
5. 抢到锁后执行内存屏障。
6. 记录当前持锁 CPU。
```

## 五、为什么 acquire 先 push_off

`push_off()` 会关闭当前 CPU 的中断。

原因是避免同一 CPU 上的中断重入死锁。

考虑这种情况：

```text
CPU0 正在内核中运行
CPU0 acquire(&lk) 成功
CPU0 还没有 release(&lk)

此时 CPU0 发生中断
中断处理函数也想 acquire(&lk)
```

如果不关中断：

```text
中断处理函数会自旋等待 lk 被释放
但释放 lk 的代码被中断打断了，无法继续执行
于是 CPU0 自己等自己，死锁
```

所以 xv6 在 `acquire()` 一开始就：

```c
push_off();
```

注意：

```text
push_off 只关闭当前 CPU 的中断。
它不能阻止其他 CPU 并发访问。
阻止其他 CPU 的是 locked 字段和原子操作。
```

## 六、holding 的作用

代码：

```c
int
holding(struct spinlock *lk)
{
  int r;
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}
```

含义：

```text
如果 locked == 1 且 lk->cpu == 当前 CPU，
说明当前 CPU 正持有这把锁。
```

`acquire()` 中：

```c
if(holding(lk))
  panic("acquire");
```

这是为了防止同一 CPU 重复获取同一把 spinlock。

xv6 的 spinlock 不是可重入锁。

错误例子：

```c
acquire(&lk);
acquire(&lk);  // panic
```

如果允许第二次 acquire，它会永远等待自己释放锁。

## 七、原子 test-and-set

`acquire()` 的核心是：

```c
while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
  ;
```

`__sync_lock_test_and_set(&lk->locked, 1)` 的语义是：

```text
原子地把 lk->locked 设置为 1。
同时返回设置前的旧值。
```

也就是：

```text
old = lk->locked;
lk->locked = 1;
return old;
```

但关键是：

```text
读取 old 和写入 1 是不可分割的原子操作。
```

如果返回旧值：

```text
0：
  说明锁之前是空闲的。
  当前 CPU 成功把它改成 1，拿到锁。

1：
  说明锁之前已经被别人持有。
  当前 CPU 没拿到锁，继续 while 自旋。
```

## 八、为什么必须是原子操作

如果不用原子操作，而是写成：

```c
while(lk->locked != 0)
  ;
lk->locked = 1;
```

会有竞态：

```text
CPU0 看到 locked == 0
CPU1 也看到 locked == 0
CPU0 设置 locked = 1
CPU1 也设置 locked = 1
CPU0 和 CPU1 都以为自己拿到了锁
```

这就破坏了互斥。

原子 test-and-set 保证：

```text
多个 CPU 同时抢锁时，只有一个 CPU 能看到旧值为 0。
其他 CPU 看到旧值为 1。
```

在 RISC-V 上，注释说明它会变成类似：

```asm
amoswap.w.aq
```

这是 atomic memory operation，原子交换指令。

## 九、自旋是什么意思

这段：

```c
while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
  ;
```

如果锁被占用，CPU 不会睡眠，也不会让出 CPU，而是不断循环尝试。

这叫自旋：

```text
spin
```

优点：

```text
实现简单。
适合临界区很短的场景。
不需要进程切换开销。
```

缺点：

```text
等待期间 CPU 一直空转。
如果锁持有时间长，会浪费 CPU。
```

所以 spinlock 适合保护短小的内核临界区，例如：

```text
修改链表指针
修改计数器
更新 ring 下标
访问 freelist
```

不适合在持锁时做长时间操作。

## 十、acquire 后的内存屏障

代码：

```c
__sync_synchronize();
```

这是 full memory barrier。

作用：

```text
禁止编译器和 CPU 把临界区里的内存访问重排到 acquire 之前。
```

为什么需要？

加锁的语义应该是：

```text
先拿到锁
再访问共享数据
```

但现代编译器和 CPU 会为了优化重排内存访问。如果没有屏障，理论上可能出现：

```text
共享数据访问被提前到锁真正获取之前
```

这会破坏锁的语义。

因此 xv6 在抢到锁后执行：

```c
__sync_synchronize();
```

保证：

```text
临界区内的 load/store 必须发生在 acquire 之后。
```

## 十一、记录持锁 CPU

代码：

```c
lk->cpu = mycpu();
```

作用：

```text
记录当前是哪一个 CPU 持有锁。
```

主要用于：

```text
holding(lk)
release(lk) 的合法性检查
调试
```

## 十二、release 的整体流程

代码：

```c
void
release(struct spinlock *lk)
{
  if(!holding(lk))
    panic("release");

  lk->cpu = 0;

  __sync_synchronize();

  __sync_lock_release(&lk->locked);

  pop_off();
}
```

整体流程：

```text
1. 检查当前 CPU 是否真的持有这把锁。
2. 清空 lk->cpu。
3. 执行内存屏障。
4. 原子释放 locked。
5. 恢复中断状态。
```

## 十三、release 为什么检查 holding

```c
if(!holding(lk))
  panic("release");
```

防止错误释放。

错误情况包括：

```text
当前 CPU 没有持有这把锁。
这把锁已经是空闲的。
这把锁由另一个 CPU 持有。
```

这些情况都说明内核锁使用逻辑出错，所以 xv6 直接 panic。

## 十四、为什么先 lk->cpu = 0

```c
lk->cpu = 0;
```

表示：

```text
从调试语义上看，这把锁不再属于当前 CPU。
```

真正释放锁的是后面的：

```c
__sync_lock_release(&lk->locked);
```

所以 `locked` 才是互斥的真实状态，`cpu` 是辅助调试字段。

## 十五、release 前的内存屏障

```c
__sync_synchronize();
```

作用：

```text
保证临界区内的所有 load/store 都在释放锁之前完成。
```

否则可能出现：

```text
CPU0 修改共享数据
CPU0 释放锁
CPU1 获取锁
CPU1 看到共享数据仍是旧值
```

加屏障后保证：

```text
临界区内的修改对其他 CPU 可见后，锁才会变成可获取状态。
```

## 十六、原子释放锁

```c
__sync_lock_release(&lk->locked);
```

语义类似：

```c
lk->locked = 0;
```

但 xv6 不直接用普通赋值，原因是：

```text
C 标准不保证普通赋值一定是单条原子 store。
这里需要和 acquire 的原子操作配合。
```

在 RISC-V 上，注释说明它会生成类似：

```asm
amoswap.w zero, zero, (s1)
```

本质上就是把 `locked` 置为 0，让其他 CPU 可以抢到锁。

## 十七、pop_off 的作用

`release()` 最后：

```c
pop_off();
```

它和 `push_off()` 配对。

代码：

```c
void
pop_off(void)
{
  struct cpu *c = mycpu();
  if(intr_get())
    panic("pop_off - interruptible");
  if(c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if(c->noff == 0 && c->intena)
    intr_on();
}
```

它不会无条件打开中断，而是：

```text
如果 push_off/pop_off 嵌套层数归零，
并且进入第一层 push_off 前中断原本是开启的，
才重新打开中断。
```

## 十八、push_off/pop_off 的嵌套计数

`push_off()`：

```c
void
push_off(void)
{
  int old = intr_get();

  intr_off();
  if(mycpu()->noff == 0)
    mycpu()->intena = old;
  mycpu()->noff += 1;
}
```

`struct cpu` 中有两个相关字段：

```text
noff：
  当前 CPU 嵌套 push_off 的层数。

intena：
  第一次 push_off 前，中断是否开启。
```

示例：

```text
初始中断开启

push_off()
  noff = 1
  intena = 1
  中断关闭

push_off()
  noff = 2
  中断仍关闭

pop_off()
  noff = 1
  不开中断

pop_off()
  noff = 0
  intena = 1
  开中断
```

这样可以安全支持嵌套锁。

## 十九、为什么 mycpu 要求关中断

`mycpu()`：

```c
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}
```

`cpuid()` 通过 `tp` 寄存器得到当前 hart id。

注释说：

```text
Interrupts must be disabled.
```

原因是：

```text
如果中断开启，当前进程可能被中断和调度影响。
关中断可以保证读取当前 CPU 状态期间不会被打断。
```

所以 `acquire()` 先 `push_off()`，之后再使用 `mycpu()` 是安全的。

## 二十、acquire/release 和多 CPU 的关系

假设两个 CPU 同时执行：

```c
acquire(&lk);
// critical section
release(&lk);
```

过程：

```text
CPU0 执行 atomic test-and-set
CPU1 也执行 atomic test-and-set

硬件保证只有一个 CPU 看到 locked 的旧值是 0。
假设 CPU0 成功。

CPU0 进入临界区。
CPU1 看到旧值是 1，在 while 中自旋。

CPU0 release，把 locked 设回 0。
CPU1 下一次 test-and-set 看到旧值是 0，成功进入临界区。
```

这就是互斥。

## 二十一、和 net 实验的关系

在 net 实验中，`e1000.c` 里有：

```c
struct spinlock e1000_lock;
```

它用于保护 E1000 驱动共享状态：

```text
tx_ring
tx_mbufs
rx_ring
rx_mbufs
regs[E1000_TDT]
regs[E1000_RDT]
```

尤其是 `e1000_transmit()`，可能被多个进程同时调用：

```text
多个进程同时 write socket
多个 CPU 同时进入 e1000_transmit
```

所以操作 TX ring 时必须加锁：

```c
acquire(&e1000_lock);

// 读取 TDT
// 检查 descriptor
// 修改 tx_ring
// 修改 tx_mbufs
// 更新 TDT

release(&e1000_lock);
```

否则可能出现：

```text
CPU0 和 CPU1 同时看到同一个 TDT
两者都往同一个 tx_ring 槽位写
一个 packet 覆盖另一个 packet
```

## 二十二、使用 spinlock 的基本规则

### 1. acquire 和 release 必须配对

错误：

```c
acquire(&lk);
if(error)
  return -1;
release(&lk);
```

正确：

```c
acquire(&lk);
if(error){
  release(&lk);
  return -1;
}
release(&lk);
```

### 2. 不要重复 acquire 同一把锁

错误：

```c
acquire(&lk);
acquire(&lk);
```

xv6 会 panic。

### 3. 不要长时间持有 spinlock

spinlock 等待时会空转。持锁时间过长会浪费 CPU。

不适合在持锁时做：

```text
可能 sleep 的操作
复杂的协议栈处理
磁盘等待
长循环
```

### 4. 不要在持有锁时调用可能再次获取同一把锁的函数

否则可能死锁。

### 5. 出错路径也要释放锁

这是最常见 bug。

例如 net 实验中：

```c
acquire(&e1000_lock);
if((tx_ring[ind].status & E1000_TXD_STAT_DD) == 0)
  return -1; // 错，漏 release
```

正确：

```c
if((tx_ring[ind].status & E1000_TXD_STAT_DD) == 0){
  release(&e1000_lock);
  return -1;
}
```

## 二十三、总结

`acquire()` 做的事情：

```text
关闭当前 CPU 中断
检查重复加锁
用原子 test-and-set 抢 locked
抢不到就自旋
执行内存屏障
记录持锁 CPU
```

`release()` 做的事情：

```text
检查当前 CPU 是否持锁
清空持锁 CPU 记录
执行内存屏障
原子地把 locked 置 0
恢复中断状态
```

三条核心原则：

```text
原子指令保证多 CPU 互斥。
关中断避免同一 CPU 上的中断重入死锁。
内存屏障保证临界区内存访问不会乱序。
```

一句话理解：

```text
spinlock 是用硬件原子指令保护共享数据的小型内核锁；
acquire 抢锁并关中断，release 释放锁并恢复中断。
```
