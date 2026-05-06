# xv6 pgtbl 实验代码阅读导引

本文档面向当前仓库的 `pgtbl` 实验，目标不是通读 xv6，而是带你沿着页表实验相关代码建立一条清晰主线。

当前实验配置在 `conf/lab.mk`：

```make
LAB=pgtbl
```

这会在编译时定义：

```text
LAB_PGTBL
SOL_PGTBL
```

因此 `Makefile` 会额外编译 `kernel/vmcopyin.c`、`kernel/stats.c`、`kernel/sprintf.c`，并把 `user/_stats` 加入用户程序。

## 1. pgtbl 实验要解决什么

MIT xv6 的 pgtbl 实验通常围绕三块内容：

```text
1. vmprint
   打印页表结构，帮助理解 Sv39 三级页表。

2. per-process kernel page table
   每个进程拥有自己的 kernel page table，而不是所有进程共用一张 kernel_pagetable。

3. simplify copyin/copyinstr
   把用户页映射进进程自己的 kernel page table，让内核可以直接通过用户虚拟地址访问用户内存。
```

这三块是递进关系：

```text
先理解页表长什么样
再让每个进程有自己的内核页表
最后把用户地址也同步映射进这张内核页表
```

## 2. 先建立页表基础

先读这些文件：

```text
kernel/memlayout.h
kernel/riscv.h
kernel/vm.c
```

### `kernel/memlayout.h`

这个文件定义 xv6 的物理地址和虚拟地址布局。

重点看：

```c
#define UART0 0x10000000L
#define VIRTIO0 0x10001000
#define CLINT 0x2000000L
#define PLIC 0x0c000000L
#define KERNBASE 0x80000000L
#define PHYSTOP (KERNBASE + 128*1024*1024)
#define TRAMPOLINE (MAXVA - PGSIZE)
#define KSTACK(p) (TRAMPOLINE - ((p)+1)* 2*PGSIZE)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
```

这些宏回答的问题是：

```text
设备寄存器在哪
内核从哪里开始
物理内存用到哪里
trampoline 放在哪
每个进程的 kernel stack 放在哪
trapframe 放在哪
```

读 `pgtbl` 实验时，尤其要记住：

```text
TRAMPOLINE 必须同时映射在 user page table 和 kernel page table
TRAPFRAME 必须映射在 user page table
KSTACK(p) 是每个进程的内核栈虚拟地址
```

### `kernel/riscv.h`

这个文件定义 RISC-V 页表相关宏和 CSR 读写函数。

重点理解：

```c
#define PGSIZE 4096
#define PX(level, va)
#define PTE_V
#define PTE_R
#define PTE_W
#define PTE_X
#define PTE_U
#define PA2PTE(pa)
#define PTE2PA(pte)
#define MAKE_SATP(pagetable)
```

你需要掌握两件事：

```text
1. 虚拟地址如何拆成 Sv39 三级页表索引。
2. PTE 里如何保存物理地址和权限位。
```

权限位的直觉：

```text
PTE_V: 这个 PTE 有效
PTE_R: 可读
PTE_W: 可写
PTE_X: 可执行
PTE_U: 用户态可以访问
```

对 pgtbl 实验很关键的一点：

```text
把用户页映射进 kernel page table 时，通常不能设置 PTE_U。
```

因为这张页表是在 supervisor mode 下使用的。内核需要能访问这些地址，但不应该把它当成用户页权限来处理。

## 3. 读 `vm.c`：页表操作核心

文件：`kernel/vm.c`

建议按这个顺序读，不要从头读到尾。

### `kvminit()`

作用：创建全局内核页表 `kernel_pagetable`。

它映射：

```text
UART0
VIRTIO0
CLINT
PLIC
kernel text
kernel data 和可用 RAM
TRAMPOLINE
```

这是一张所有 CPU 启动时都会使用的内核页表。

当前 pgtbl 实验的核心是：后面不再只依赖这张全局内核页表，而是让每个进程有一张类似的内核页表。

### `kvminithart()`

作用：让当前 CPU 使用 `kernel_pagetable`。

核心代码：

```c
w_satp(MAKE_SATP(kernel_pagetable));
sfence_vma();
```

`satp` 是每个 CPU 自己的 CSR，所以每个 CPU 都要调用一次。

在 per-process kernel page table 实验里，你会看到 scheduler 切换进程时也会写 `satp`，把当前 CPU 切到进程自己的 `kpagetable`。

### `walk()`

作用：在三级页表里查找某个虚拟地址对应的 PTE。

关键参数：

```c
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc)
```

```text
pagetable: 根页表
va: 要查找的虚拟地址
alloc: 中间页表页不存在时是否分配
```

`walk()` 是整个页表系统的核心。`mappages()`、`walkaddr()`、`uvmunmap()` 都依赖它。

读法：

```text
level = 2 查根页表
level = 1 查中间页表
level = 0 返回最终 PTE 地址
```

### `mappages()`

作用：建立虚拟地址到物理地址的映射。

核心行为：

```text
对每一页：
  用 walk(..., alloc=1) 找到或创建 PTE
  检查是否已经映射
  写入 PA2PTE(pa) | perm | PTE_V
```

所有页表映射基本都通过它完成，包括：

```text
kvmmap()
ukvmmap()
proc_pagetable()
uvmalloc()
```

### `walkaddr()`

作用：在用户页表里把用户虚拟地址翻译成物理地址。

它会检查：

```text
PTE_V 必须有效
PTE_U 必须存在
```

这说明它只适合检查用户页表。后续 `copyin_new()` 的目标就是减少这种手工翻译。

### `uvmalloc()` / `uvmdealloc()`

作用：扩展或收缩用户地址空间。

`sbrk()` 最终会走到 `growproc()`，再调用这两个函数。

对 pgtbl 实验很关键：

```text
如果用户地址空间增长，进程的 kpagetable 也要同步增加用户页映射。
如果用户地址空间收缩，进程的 kpagetable 也要同步删除对应用户页映射。
```

否则 `copyin_new()` 直接访问用户虚拟地址时，内核页表里找不到映射。

### `uvmcopy()`

作用：`fork()` 时复制父进程用户内存到子进程用户页表。

它会重新分配物理页并复制内容。

对 per-process kernel page table 来说，子进程还需要把这些新物理页同步映射进自己的 `kpagetable`。

### `uvmunmap()` / `freewalk()` / `uvmfree()`

作用：解除映射并释放页表。

要区分两类释放：

```text
用户页表释放:
  需要释放用户物理页。

进程内核页表释放:
  不能释放被直接映射的设备、内核代码、内核数据、用户物理页。
  通常只释放页表页本身。
```

这也是为什么 pgtbl 实验里通常会新增类似 `kvmfree()` 或 `freewalk` 变体。

### `vmprint()`

作用：打印三级页表结构。

当前仓库已经有实现：

```c
void vmprint(pagetable_t pagetable)
```

阅读目标：

```text
它按 level 2 -> level 1 -> level 0 遍历页表。
每遇到 PTE_V，就打印 PTE 和物理地址。
```

建议对照 `exec.c`：

```c
if(p->pid==1)
  vmprint(p->pagetable);
```

这表示第一个执行 `exec()` 的进程会打印自己的用户页表。

## 4. 读 `proc.h`：进程结构里新增了什么

文件：`kernel/proc.h`

重点看 `struct proc`：

```c
pagetable_t pagetable;
uint64 kstack;
struct trapframe *trapframe;
struct context context;
pagetable_t kpagetable;
```

原版 xv6 每个进程有：

```text
pagetable: 用户页表
kstack: 内核栈虚拟地址
trapframe: 保存用户寄存器
context: 内核线程上下文
```

当前仓库新增：

```text
kpagetable: 进程自己的内核页表
```

读 pgtbl 实验时，可以把一个进程理解成同时有两张重要页表：

```text
p->pagetable
  用户态运行时使用。
  包含用户代码、数据、栈、TRAPFRAME、TRAMPOLINE。

p->kpagetable
  该进程在内核态运行时使用。
  包含内核直接映射、设备映射、TRAMPOLINE、该进程 kernel stack。
  为 simplify copyin，还需要包含该进程用户页映射。
```

## 5. 读 `proc.c`：进程生命周期和页表切换

文件：`kernel/proc.c`

这是 pgtbl 实验最重要的文件之一。

### `procinit()`

作用：初始化进程表，并为每个进程槽分配 kernel stack。

原始逻辑：

```text
遍历 proc[NPROC]
初始化 p->lock
分配一页物理内存作为 kernel stack
把它映射到全局 kernel_pagetable 的 KSTACK(i)
保存 p->kstack
```

对 per-process kernel page table 来说，每个进程自己的 `kpagetable` 也必须映射自己的 kernel stack。

### `allocproc()`

作用：找到一个 UNUSED 进程槽，把它初始化成可运行进程的基础结构。

它会分配：

```text
pid
trapframe
user pagetable
kernel context
```

当前仓库还试图在这里初始化：

```text
p->kpagetable
```

阅读时重点关注顺序：

```text
trapframe 必须先有，proc_pagetable() 才能映射 TRAPFRAME。
kpagetable 必须先有，才能往里面映射 kernel stack / TRAMPOLINE / 用户页。
```

当前仓库这里存在一个需要你核对的点：

```text
allocproc() 先调用 proc_pagetable(p)，后初始化 p->kpagetable。
但 proc_pagetable() 内部又访问 p->kpagetable。
这个顺序是不安全的。
```

这类问题是 pgtbl 实验最常见 bug：页表初始化职责边界不清。

### `proc_pagetable()`

作用：创建用户页表，并映射：

```text
TRAMPOLINE
TRAPFRAME
```

原本它只负责 `p->pagetable`。如果你把 `p->kpagetable` 的映射也塞进这里，要非常小心调用顺序。

建议阅读时问自己：

```text
这个函数到底只创建 user pagetable，还是也修改 kernel pagetable？
如果它也修改 kpagetable，调用它之前 kpagetable 是否已经存在？
失败时释放路径是否完整？
```

### `freeproc()`

作用：释放进程持有的资源，并把状态改回 `UNUSED`。

pgtbl 实验新增 `kpagetable` 后，必须释放：

```text
p->kpagetable 的页表页
```

但注意不能释放：

```text
内核代码物理页
设备寄存器
用户物理页，除非它们由 user pagetable 的释放路径负责
```

### `growproc()`

作用：实现进程地址空间增长/收缩，`sbrk()` 会走到这里。

原始逻辑只更新：

```text
p->pagetable
p->sz
```

对 `copyin_new()` 来说不够。因为内核之后要通过 `p->kpagetable` 直接访问用户虚拟地址，所以这里也要同步：

```text
n > 0: 把新增用户页映射进 p->kpagetable
n < 0: 从 p->kpagetable 删除缩掉的用户页映射
```

### `fork()`

作用：复制父进程，创建子进程。

原始页表逻辑：

```text
allocproc() 创建子进程基础结构
uvmcopy(parent->pagetable, child->pagetable, parent->sz)
child->sz = parent->sz
```

pgtbl 实验还需要：

```text
把 child->pagetable 中的用户页映射同步进 child->kpagetable
```

当前仓库有 `ukvmcopy(struct proc *p)`，看起来就是为这个目的准备的。

阅读时注意：

```text
fork() 里不能只是重新 ukvminit()
因为 allocproc() 已经应该创建 child->kpagetable。
真正需要的是把 child 用户页映射进去。
```

### `scheduler()`

作用：每个 CPU 循环寻找 RUNNABLE 进程并切换运行。

per-process kernel page table 的关键改动在这里：

```c
w_satp(MAKE_SATP(p->kpagetable));
sfence_vma();
swtch(&c->context, &p->context);
kvminithart();
```

含义：

```text
运行某个进程的内核线程时，CPU 使用该进程自己的 kpagetable。
进程让出 CPU 回到 scheduler 后，恢复全局 kernel_pagetable。
```

这个逻辑成立的前提：

```text
p->kpagetable 必须包含 scheduler 后续要访问的内核代码、内核数据、设备映射。
p->kpagetable 必须包含当前进程 kernel stack。
TRAMPOLINE 必须可用。
```

## 6. 读 `exec.c`：替换用户地址空间

文件：`kernel/exec.c`

`exec()` 会丢弃当前进程旧的用户地址空间，加载一个新程序。

关键流程：

```text
创建新 user pagetable
读取 ELF header
为程序段分配用户页
loadseg() 把 ELF 内容读入用户页
分配用户栈
提交新 pagetable 到 p->pagetable
释放旧 pagetable
```

对 pgtbl 实验非常关键：

```text
如果 p->pagetable 被替换，p->kpagetable 里旧的用户页映射也必须替换。
```

否则会出现：

```text
p->pagetable 指向新程序
p->kpagetable 仍映射旧程序的用户页
copyin_new/copyinstr_new 访问到错误内存
```

当前仓库在 `exec()` 里有：

```c
if(p->pid==1)
  vmprint(p->pagetable);
```

这是 `vmprint` 实验常见要求。

## 7. 读 `trap.c` 和 `trampoline.S`：为什么 TRAMPOLINE/TRAPFRAME 重要

文件：

```text
kernel/trap.c
kernel/trampoline.S
```

### `usertrapret()`

从内核返回用户态前，会设置：

```c
p->trapframe->kernel_satp = r_satp();
p->trapframe->kernel_sp = p->kstack + PGSIZE;
p->trapframe->kernel_trap = (uint64)usertrap;
p->trapframe->kernel_hartid = r_tp();
```

这些值会被 `trampoline.S:uservec` 使用。

在 per-process kernel page table 下，这里的：

```c
p->trapframe->kernel_satp = r_satp();
```

应该保存当前进程的 kernel page table。这样用户态 trap 进内核时，`uservec` 能切回正确的内核页表。

### `trampoline.S:uservec`

用户态发生系统调用、中断或异常时，会先进入 `uservec`。

注意此时：

```text
CPU 已进入 supervisor mode
但 satp 仍然是用户页表
```

所以 `uservec` 必须：

```text
保存用户寄存器到 TRAPFRAME
从 trapframe 读出 kernel_sp / kernel_trap / kernel_satp
切换 satp 到 kernel_satp
跳到 usertrap()
```

因此：

```text
TRAMPOLINE 必须在 user pagetable 和 kernel pagetable 中映射到同一个虚拟地址。
TRAPFRAME 必须能在 user pagetable 中被 trampoline 访问。
```

## 8. 读 `vmcopyin.c`：simplify copyin 的目标

文件：`kernel/vmcopyin.c`

这里提供：

```c
copyin_new()
copyinstr_new()
```

原始 `copyin()` 的做法：

```text
用 walkaddr() 手工把用户虚拟地址翻译成物理地址
再 memmove
```

`copyin_new()` 的做法：

```c
memmove((void *) dst, (void *)srcva, len);
```

它直接把用户虚拟地址当成内核可访问地址。

这只有在一个条件下成立：

```text
当前 CPU 使用的 kernel page table 中，已经映射了当前进程的用户页。
```

所以 `copyin_new()` 依赖前面所有工作：

```text
allocproc/fork/exec/growproc 都要维护 p->kpagetable 中的用户映射。
scheduler 要切到 p->kpagetable。
```

最后还要把 `vm.c` 里的 `copyin()` / `copyinstr()` 改成调用：

```text
copyin_new()
copyinstr_new()
```

当前仓库的 `vm.c` 里还保留旧实现，这是你继续实验时要核对的点。

## 9. 推荐阅读路线

### 第一轮：只理解原版页表机制

```text
kernel/memlayout.h
kernel/riscv.h
kernel/vm.c: kvminit
kernel/vm.c: walk
kernel/vm.c: mappages
kernel/vm.c: walkaddr
kernel/vm.c: uvmalloc
kernel/vm.c: uvmunmap
```

这一轮目标：

```text
能说清楚 xv6 怎么建立一个 VA -> PA 映射。
```

### 第二轮：理解用户进程页表

```text
kernel/proc.c: allocproc
kernel/proc.c: proc_pagetable
kernel/proc.c: userinit
kernel/exec.c: exec
kernel/trampoline.S: uservec/userret
kernel/trap.c: usertrapret
```

这一轮目标：

```text
能说清楚一个进程的 user pagetable 包含哪些映射，以及 trap 进出内核为什么依赖 TRAMPOLINE/TRAPFRAME。
```

### 第三轮：理解 per-process kernel page table

```text
kernel/proc.h: struct proc.kpagetable
kernel/vm.c: ukvminit
kernel/vm.c: ukvmmap
kernel/vm.c: ukvmunmap
kernel/vm.c: ufreewalk/uprockvmfree
kernel/proc.c: allocproc
kernel/proc.c: freeproc
kernel/proc.c: scheduler
```

这一轮目标：

```text
能说清楚 p->kpagetable 应该包含哪些映射，以及 CPU 什么时候切到它。
```

### 第四轮：理解用户页同步到 kpagetable

```text
kernel/proc.c: fork
kernel/proc.c: growproc
kernel/exec.c: exec
kernel/vm.c: ukvmcopy
kernel/vmcopyin.c: copyin_new/copyinstr_new
```

这一轮目标：

```text
能说清楚用户地址空间变化时，p->kpagetable 该如何同步。
```

## 10. 当前仓库的重点核对清单

下面这些不是要求你立刻修改，而是读代码时要重点验证的地方。

### 初始化顺序

检查：

```text
p->kpagetable 是否在任何使用它之前已经创建。
proc_pagetable() 是否不小心依赖了未初始化的 p->kpagetable。
```

当前代码里这一点值得重点检查。

### 重复映射

检查：

```text
TRAMPOLINE 是否被同一张页表映射了两次。
```

如果 `mappages()` 遇到已有有效 PTE，会 `panic("remap")`。

### fork 同步

检查：

```text
fork() 复制完 child->pagetable 后，是否把 child 用户页映射进 child->kpagetable。
```

### exec 替换

检查：

```text
exec() 替换 p->pagetable 后，p->kpagetable 中的用户页映射是否也替换。
```

### sbrk 增减

检查：

```text
growproc() 增加/减少用户页时，p->kpagetable 是否同步更新。
```

### copyin 接入

检查：

```text
vm.c:copyin 是否调用 copyin_new。
vm.c:copyinstr 是否调用 copyinstr_new。
```

### PTE 权限

检查：

```text
用户页映射进 p->kpagetable 时，不应设置 PTE_U。
```

## 11. 用问题驱动阅读

每读完一段代码，尝试回答这些问题：

```text
1. 当前 CPU 此刻使用的是哪张页表？
2. 这张页表里有没有 TRAMPOLINE？
3. 当前进程的 kernel stack 是否在这张页表里？
4. 当前进程的用户页是否在这张页表里？
5. 如果进程 fork/exec/sbrk，这些映射会不会失效？
6. 释放页表时，哪些物理页应该释放，哪些不能释放？
```

如果这六个问题都能回答，pgtbl 实验主线基本就通了。
