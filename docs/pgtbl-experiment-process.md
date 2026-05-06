# pgtbl 实验过程记录

本文档记录本次 pgtbl 实验的阅读、实现、调试过程。重点不是只列最终改了哪些文件，而是说明每个问题是如何暴露的、根因是什么、当前代码用什么方式解决。

## 一、实验目标

pgtbl 实验主要分三部分：

1. 实现 `vmprint()`，打印页表结构。
2. 实现每个进程一张内核页表，即 `p->kpagetable`。
3. 把用户页映射同步到 `p->kpagetable`，使内核可以通过新版 `copyin_new/copyinstr_new` 直接访问用户虚拟地址。

本实验的关键不变量是：

```text
p->pagetable 中存在的用户页映射，
需要同步到 p->kpagetable 中。

同步到 p->kpagetable 时，
必须清掉 PTE_U。
```

原因是 `p->kpagetable` 是内核态使用的页表，用户态不应该通过这张页表获得权限。

## 二、启动和基础概念梳理

实验前先阅读了启动流程：

```text
entry.S
start.c
main.c
proc.c
vm.c
trap.c
trampoline.S
```

确认了几个核心概念：

```text
satp:
  CPU 当前使用的根页表物理地址。

CSR:
  RISC-V 控制状态寄存器，例如 satp、sepc、scause、stval。

mret/sret:
  从 machine/supervisor trap 返回。

timer interrupt:
  时钟中断，用来驱动调度。

TLB:
  CPU 中缓存 VA -> PA 翻译结果的结构。页表变化后需要 sfence_vma() 刷新。
```

还确认了多 CPU 情况：

```text
每个 hart/CPU 有自己的一套寄存器和 CSR。
w_mscratch、w_satp 等写的是当前 CPU 的 CSR，不会直接覆盖其他 CPU 的 CSR。
```

## 三、vmprint 实现

相关文件：

```text
kernel/vm.c
kernel/defs.h
kernel/exec.c
```

当前实现新增：

```c
void vmprint(pagetable_t pagetable);
static void vmprint_walk(pagetable_t pagetable, int depth);
```

实现逻辑：

```text
1. 打印根页表地址。
2. 遍历 512 个 PTE。
3. 只打印 PTE_V 有效的项。
4. 如果 PTE 没有 R/W/X，说明它指向下一级页表，递归打印。
5. 如果 PTE 有 R/W/X，说明它是 leaf PTE，打印 pte 和 pa。
```

判断 leaf 的核心逻辑：

```c
if((pte & (PTE_R | PTE_W | PTE_X)) == 0)
```

在 `exec()` 中对 `pid == 1` 的进程调用：

```c
if(p->pid==1)
  vmprint(p->pagetable);
```

该部分后来通过了 pte printout 相关测试。

## 四、每进程内核页表

### 1. `struct proc` 增加字段

文件：

```text
kernel/proc.h
```

新增：

```c
pagetable_t kpagetable;
```

含义：

每个进程拥有自己的内核页表。进程在内核态运行时，不再只使用全局 `kernel_pagetable`。

### 2. `ukvminit()`

文件：

```text
kernel/vm.c
```

新增：

```c
pagetable_t ukvminit(void);
```

作用：

创建一张进程内核页表，并映射内核运行需要的公共区域：

```text
UART0
VIRTIO0
PLIC
kernel text
kernel data
TRAMPOLINE
```

调试过程中曾经把 `CLINT` 也映射进 `p->kpagetable`，后来发现这会限制用户地址空间增长，于是移除了每进程内核页表中的 `CLINT` 映射。

保留全局 `kernel_pagetable` 中的 `CLINT` 映射，但不在每进程内核页表中映射它。

### 3. `allocproc()` 中创建 `p->kpagetable`

文件：

```text
kernel/proc.c
```

新增逻辑：

```c
p->kpagetable = ukvminit();
uint64 va = KSTACK((int)(p - proc));
pte_t pa = kvmpa(va);
memset((void *)pa, 0, PGSIZE);
ukvmmap(p->kpagetable, va, pa, PGSIZE, PTE_R | PTE_W);
p->kstack = va;
```

作用：

```text
1. 为进程创建自己的内核页表。
2. 找到该进程 kernel stack 的虚拟地址。
3. 通过全局内核页表找到 kernel stack 的物理地址。
4. 把这个 kernel stack 映射进 p->kpagetable。
```

这里读懂了一个关键点：

```text
kernel stack 不是用户第 0 页。
allocproc() 不会创建用户页。
用户第 0 页来自 userinit()/uvminit() 或 exec()/uvmalloc()。
```

### 4. `scheduler()` 切换页表

文件：

```text
kernel/proc.c
```

进程被调度运行前：

```c
w_satp(MAKE_SATP(p->kpagetable));
sfence_vma();
swtch(&c->context, &p->context);
kvminithart();
```

含义：

```text
进入进程的内核上下文时，使用 p->kpagetable。
进程切回调度器后，恢复全局 kernel_pagetable。
```

这里也确认了 TLB 刷新的必要性：

```text
写 satp 后必须 sfence_vma()。
```

## 五、用户映射同步到 `p->kpagetable`

### 1. `ukvmcopyrange()`

文件：

```text
kernel/vm.c
```

新增：

```c
int ukvmcopyrange(pagetable_t upagetable,
                  pagetable_t kpagetable,
                  uint64 oldsz,
                  uint64 newsz);
```

作用：

把用户页表中某个范围的用户页映射同步到进程内核页表。

同步范围：

```text
[PGROUNDUP(oldsz), newsz)
```

核心逻辑：

```c
pte = walk(upagetable, a, 0);
pa = PTE2PA(*pte);
flags = PTE_FLAGS(*pte) & ~PTE_U;
mappages(kpagetable, a, PGSIZE, pa, flags);
```

同步时清掉 `PTE_U`：

```text
用户页表里用户页带 PTE_U。
内核页表中的同一页映射不应该带 PTE_U。
```

当前限制：

```text
用户地址空间不能超过 PLIC。
```

原因：

`p->kpagetable` 里有设备映射。用户 VA 如果增长到设备地址范围，会和设备映射冲突，触发 `remap`。

### 2. `userinit()` 同步第一个用户页

文件：

```text
kernel/proc.c
```

`userinit()` 调用：

```c
uvminit(p->pagetable, initcode, sizeof(initcode));
p->sz = PGSIZE;
ukvmcopyrange(p->pagetable, p->kpagetable, 0, PGSIZE);
```

原因：

第一个用户进程不是 fork 出来的，而是 `userinit()` 手动创建。`uvminit()` 只建立了：

```text
p->pagetable: VA 0 -> initcode 物理页
```

所以还需要同步到：

```text
p->kpagetable: VA 0 -> 同一个物理页
```

### 3. `fork()` 同步子进程用户映射

文件：

```text
kernel/proc.c
```

当前逻辑：

```c
uvmcopy(p->pagetable, np->pagetable, p->sz);
np->sz = p->sz;
ukvmcopyrange(np->pagetable, np->kpagetable, 0, p->sz);
```

调试中曾经讨论过起始地址不能从 `PGSIZE` 开始。

原因：

```text
fork() 创建的是新的子进程 kpagetable。
它里面还没有任何用户页映射。
必须同步完整 [0, np->sz)，包括用户第 0 页。
```

### 4. `growproc()` 处理 sbrk 增长和缩小

文件：

```text
kernel/proc.c
```

增长时：

```c
oldsz = p->sz;
sz = uvmalloc(p->pagetable, sz, sz + n);
ukvmcopyrange(p->pagetable, p->kpagetable, oldsz, sz);
sfence_vma();
```

缩小时：

```c
ukvmunmap(p->kpagetable,
          PGROUNDUP(sz+n),
          (PGROUNDUP(sz)-PGROUNDUP(sz+n))/PGSIZE);
sfence_vma();
sz = uvmdealloc(p->pagetable, sz, sz + n);
```

这里明确了：

```text
p->sz 不一定是 PGSIZE 的倍数。
页表操作必须使用 PGROUNDUP/PGROUNDDOWN 处理边界。
```

### 5. `exec()` 替换用户页表后重新同步

文件：

```text
kernel/exec.c
```

`exec()` 会创建全新的用户页表，所以需要：

```text
1. 删除 p->kpagetable 中旧的用户映射。
2. 提交新的 p->pagetable 和 p->sz。
3. 把新用户页表同步到 p->kpagetable。
```

当前逻辑：

```c
oldpagetable = p->pagetable;
oldsz = p->sz;

if(oldsz > 0)
  ukvmunmap(p->kpagetable, 0, PGROUNDUP(oldsz)/PGSIZE);

p->pagetable = pagetable;
p->sz = sz;

if(ukvmcopyrange(p->pagetable, p->kpagetable, 0, p->sz) < 0)
  panic("exec: ukvmcopyrange");
```

## 六、`copyin/copyinstr` 切换到新版

文件：

```text
kernel/vm.c
kernel/vmcopyin.c
kernel/defs.h
```

原始 `copyin()` 会手动查用户页表：

```text
walkaddr()
PA
memmove()
```

实验要求切换到新版：

```c
copyin()     -> copyin_new()
copyinstr()  -> copyinstr_new()
```

新版直接访问用户虚拟地址：

```c
memmove(dst, (void *)srcva, len);
```

因此它依赖：

```text
当前 satp 必须是 p->kpagetable。
p->kpagetable 必须含有 srcva 对应的用户映射。
```

调试中遇到过：

```text
scause 0xd
sepc = memmove()
stval = 某个用户地址
panic: kerneltrap
```

这说明内核态直接访问用户 VA 时失败。排查后确认需要：

```text
1. usertrapret() 中保存 p->kpagetable 到 trapframe->kernel_satp。
2. growproc()/exec() 修改当前 p->kpagetable 后刷新 TLB。
3. copyin_new/copyinstr_new 访问前检查用户地址是否真的映射。
```

当前 `copyin_new/copyinstr_new` 已加入部分 `walkaddr()` 检查，用于避免 `validatetest` 传入未映射地址时导致内核 panic。

仍需注意：

```text
copyin_new 的边界条件应该使用 srcva + len > p->sz，而不是 >=。
copyin_new 需要正确处理 len == 0，避免 srcva+len-1 下溢。
copyinstr_new 应该按访问到的每一页检查，而不是始终检查 srcva 所在页。
```

## 七、释放页表和物理页

本实验过程中重点区分了两类释放：

```text
unmap:
  删除 leaf PTE。
  根据 do_free 决定是否释放 leaf 指向的物理页。

freewalk:
  释放页表页本身。
  要求所有 leaf PTE 已经被清掉。
```

### 用户页表释放

`proc_freepagetable()`：

```c
uvmunmap(pagetable, TRAMPOLINE, 1, 0);
uvmunmap(pagetable, TRAPFRAME, 1, 0);
uvmfree(pagetable, sz);
```

`uvmfree()`：

```c
uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
freewalk(pagetable);
```

含义：

```text
TRAMPOLINE/TRAPFRAME 只取消映射，不释放物理页。
用户物理页由 uvmunmap(..., do_free=1) 释放。
页表页由 freewalk() 释放。
```

### 进程内核页表释放

`p->kpagetable` 中的映射大多是借用：

```text
用户页映射
kernel stack
kernel text/data
设备映射
TRAMPOLINE
```

所以释放时使用 `ukvmunmap()`：

```text
只清 PTE，不 kfree(pa)。
```

当前 `freeproc()` 里先删除 `p->kpagetable` 里的用户映射：

```c
ukvmunmap(p->kpagetable, 0, PGROUNDUP(sz)/PGSIZE);
```

再调用：

```c
uprockvmfree(p);
```

`uprockvmfree()` 清掉内核相关映射后调用：

```c
ufreewalk(kpagetable);
```

## 八、调试中遇到的问题和解决

### 问题 1：`remap va=0x02000000`

现象：

```text
remap va=0x0000000002000000
oldpa=0x0000000002000000
panic: remap
```

原因：

`0x02000000` 是 `CLINT`。当用户地址空间增长到这个地址时，`ukvmcopyrange()` 试图把用户页映射到 `p->kpagetable` 的 `0x02000000`，但该地址已经映射了 `CLINT`。

解决：

```text
从 ukvminit() 中移除 CLINT 映射。
从 uprockvmfree() 中移除 CLINT unmap。
ukvmcopyrange() 上限改为 PLIC。
```

结果：

用户地址空间可以增长到更高，避免过早撞到 `CLINT`。

### 问题 2：`panic: growproc : ukvmcopyrange`

现象：

```text
panic: growproc : ukvmcopyrange
```

原因：

`ukvmcopyrange()` 返回 `-1` 时，`growproc()` 直接 `panic`。但用户程序申请过大内存是正常失败路径，不应该让内核崩溃。

解决：

```c
if(ukvmcopyrange(...) < 0){
  uvmdealloc(p->pagetable, sz, oldsz);
  return -1;
}
```

注意：

这里曾经把 `uvmdealloc()` 参数写反：

```c
uvmdealloc(p->pagetable, oldsz, sz);
```

这是错误的。正确是：

```c
uvmdealloc(p->pagetable, sz, oldsz);
```

因为 `uvmdealloc(pagetable, oldsz, newsz)` 中第一个 size 应该是较大的旧大小，第二个是缩小后的新大小。

### 问题 3：`panic: freewalk: leaf`

现象：

```text
pa=0x0000000085dff000
panic: freewalk: leaf
```

原因：

`freewalk()` 释放页表页时，发现页表里还有 leaf PTE。`0x85dff000` 看起来是用户物理页，不是 `TRAPFRAME` 或 `TRAMPOLINE`。

最终定位到一个重要原因：

```text
growproc() 失败回滚时 uvmdealloc() 参数写反，
导致 uvmalloc() 刚分配的用户页没有被释放，
用户页表里残留 leaf PTE。
```

解决：

```c
uvmdealloc(p->pagetable, sz, oldsz);
```

同时明确释放页表的规则：

```text
释放整张页表前必须先 unmap 所有 leaf PTE。
freewalk() 只释放页表页，不负责释放 leaf 映射。
```

### 问题 4：`scause=0xd sepc=memmove stval=用户地址`

现象：

```text
scause 0x000000000000000d
sepc=0x0000000080000d74
stval=0x0000000000013f58
panic: kerneltrap
```

定位：

`sepc` 对应 `memmove()`，说明新版 `copyin_new()` 在内核态直接读用户地址时 page fault。

原因：

用户态 trap 进入内核时，`trampoline.S` 会从 `trapframe->kernel_satp` 恢复内核页表。如果这里保存的是全局 `kernel_pagetable`，而不是 `p->kpagetable`，那么 `copyin_new()` 直接访问用户 VA 就会失败。

解决：

文件：

```text
kernel/trap.c
```

修改：

```c
p->trapframe->kernel_satp = MAKE_SATP(p->kpagetable);
```

同时在 `growproc()` 修改当前进程内核页表后加入：

```c
sfence_vma();
```

### 问题 5：`validatetest` 触发 `kerneltrap`

现象：

```text
test validatetest:
scause 0xd
stval=0x000000000000f000
panic: kerneltrap
```

原因：

`validatetest` 会传入小于 `p->sz` 但实际未映射的用户地址。只检查：

```c
srcva < p->sz
```

不够。地址小于 `p->sz` 不代表该页真的有 PTE。

解决思路：

在 `copyin_new/copyinstr_new` 直接读用户地址前，通过 `walkaddr()` 检查将要访问的用户页是否存在。

当前代码已经加入了部分检查，但仍应继续完善边界：

```text
copyin_new:
  检查复制范围覆盖到的每一页。

copyinstr_new:
  每次跨页时检查新页是否映射。
```

### 问题 6：`sbrkmuch` 失败

现象：

```text
test sbrkmuch:
sbrkmuch: sbrk test failed to grow big address space; enough phys mem?
FAILED
```

原因：

最初 `ukvmcopyrange()` 用 `CLINT` 作为上限：

```c
if(newsz > CLINT)
  return -1;
```

这使用户地址空间只能增长到 `0x02000000`，对 `sbrkmuch` 来说太小。

解决：

```text
不在 p->kpagetable 映射 CLINT。
将上限改为 PLIC。
```

## 九、当前 usertests 输出解读

已经通过的部分说明：

```text
execout/copyin/copyout/copyinstr:
  基础系统调用和新版 copyin 路径基本工作。

fork/exec/file tests:
  fork、exec、文件系统路径基本可用。

sbrkbasic:
  基础 sbrk 增长和缩小可运行。
```

`sbrkbugs` 中出现的用户态 fault 最后是 `OK`：

```text
这是用户程序故意触发非法执行或非法访问，内核杀掉子进程即可。
```

`kernmem` 中大量 `stval=0x80000000` 也是预期：

```text
用户程序尝试访问内核地址，应该 fault。
最后 OK 表示行为正确。
```

真正需要关注的是：

```text
sbrkmuch FAILED:
  用户空间增长上限或内存回滚逻辑仍需验证。

validatetest kerneltrap:
  copyin_new/copyinstr_new 对未映射用户页的检查仍需完善。
```

## 十、当前代码状态

当前已经完成或基本完成：

```text
vmprint
struct proc 增加 kpagetable
ukvminit/ukvmmap/ukvmunmap/ufreewalk/uprockvmfree
allocproc 映射每进程 kernel stack
scheduler 切换到 p->kpagetable
userinit/fork/growproc/exec 同步用户映射
copyin/copyinstr 切到新版
trapframe->kernel_satp 使用 p->kpagetable
移除每进程内核页表中的 CLINT 映射
growproc 失败回滚修正
部分 TLB 刷新
```

仍需重点检查：

```text
1. ukvminit() 需要检查 kalloc() 是否返回 0。
2. freeproc() 中最好先 proc_freepagetable() 再 kfree(p->trapframe)，生命周期更清晰。
3. copyin_new() 的边界应处理 len == 0，并使用 > 而不是 >=。
4. copyin_new() 检查页范围时应使用 uint64，避免 int 截断。
5. copyinstr_new() 应按跨页检查，而不是反复检查 srcva 所在页。
6. exec() 中 ukvmcopyrange() 失败目前 panic，更完整实现应做失败恢复。
7. 需要重新跑 validatetest、sbrkmuch、完整 usertests 和 make grade。
```

## 十一、最终理解

本实验的核心不是简单“复制一张内核页表”，而是维护一组一致性关系：

```text
进程创建:
  创建 p->kpagetable，并映射 kernel stack。

进程运行:
  scheduler 切换 satp 到 p->kpagetable。

用户态 trap 进内核:
  trampoline 使用 trapframe->kernel_satp 切回 p->kpagetable。

用户地址空间变化:
  同步维护 p->pagetable 和 p->kpagetable。

进程退出:
  用户页表释放用户物理页。
  进程内核页表只删除借用映射并释放页表页。
```

只要某个路径漏掉同步、释放顺序错误、TLB 没刷新，或者直接访问了未映射用户页，就会出现：

```text
remap
freewalk: leaf
kerneltrap
usertests failed
```

这些错误都可以通过 `scause/sepc/stval`、`mappages()` 打印、`freewalk()` 打印和 `walkaddr()` 检查逐步定位。
