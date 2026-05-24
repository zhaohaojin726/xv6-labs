# mmap 分支工作总结

当前分支：`mmap`

当前实验：xv6-labs-2020 `mmap`

本文档总结当前分支已经完成的主要修改、设计思路、遇到的问题和后续需要重点验证的地方。

## 一、实验目标

本实验实现两个系统调用：

```c
void *mmap(void *addr, int length, int prot, int flags, int fd, int offset);
int munmap(void *addr, int length);
```

目标是让用户进程可以把文件内容映射到自己的虚拟地址空间中：

```c
char *p = mmap(0, PGSIZE * 2, PROT_READ, MAP_PRIVATE, fd, 0);
```

之后用户访问 `p[i]` 时，如果页面还没有实际加载，会触发 page fault，内核再从文件中读取对应内容并建立页表映射。

## 二、系统调用入口

修改文件：

```text
kernel/syscall.h
kernel/syscall.c
user/user.h
user/usys.pl
```

新增系统调用号：

```c
#define SYS_mmap   22
#define SYS_munmap 23
```

在 syscall 表中注册：

```c
extern uint64 sys_mmap(void);
extern uint64 sys_munmap(void);

[SYS_mmap]    sys_mmap,
[SYS_munmap]  sys_munmap,
```

用户态声明：

```c
void* mmap(void *addr, int length, int prot, int flags, int fd, int offset);
int munmap(void *addr, int length);
```

这里 `mmap()` 必须返回 `void *`。之前写成 `int` 会导致 `mmaptest.c` 编译错误：

```text
initialization of 'char *' from 'int' makes pointer from integer without a cast
```

用户态 syscall stub：

```perl
entry("mmap");
entry("munmap");
```

## 三、用户测试程序

修改文件：`Makefile`

将 `mmaptest` 加入用户程序：

```make
$U/_mmaptest\
```

这样 `make qemu` 生成 `fs.img` 时会把 `mmaptest` 放入 xv6 文件系统。

## 四、PROT 和 MAP 标志

修改文件：`kernel/fcntl.h`

当前定义：

```c
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
```

含义：

```text
prot  决定映射页的访问权限。
flags 决定写入 mmap 区域后是否写回文件。
```

映射到页表权限：

```text
PROT_READ   -> PTE_R
PROT_WRITE  -> PTE_W
PROT_EXEC   -> PTE_X
所有用户页都需要 PTE_U
```

`MAP_SHARED`：

```text
解除映射时需要把已映射页面写回文件。
```

`MAP_PRIVATE`：

```text
修改只影响当前进程内存，不写回文件。
```

## 五、VMA 数据结构

修改文件：`kernel/proc.h`

新增：

```c
#define MAXVMA 16

struct vma {
  int used;
  uint64 addr;
  int offset;
  struct file *file;
  int flags;
  int prot;
  int length;
};
```

在 `struct proc` 中新增：

```c
struct vma vmas[MAXVMA];
uint64 mmapaddr;
```

含义：

```text
vmas[]   记录当前进程拥有的 mmap 区域。
mmapaddr 下一次 mmap 从哪里分配虚拟地址。
```

当前策略：

```text
mmapaddr 初始化为 TRAPFRAME。
每次 mmap 从高地址向低地址分配。
```

这样 mmap 区域和低地址的 text/data/heap 分离，避免和 `sbrk()` 管理的 `p->sz` 冲突。

## 六、进程生命周期中的 mmap 状态

修改文件：`kernel/proc.c`

### allocproc

在新进程创建时初始化：

```c
p->mmapaddr = TRAPFRAME;
```

`TRAPFRAME` 是用户地址空间顶部附近的特殊页。当前分配策略是先减再用：

```c
p->mmapaddr -= len;
addr = p->mmapaddr;
```

所以第一次 mmap 不会覆盖 `TRAPFRAME`。

### freeproc

在进程结构复用前清理：

```c
p->mmapaddr = 0;
```

这个清理不是释放资源，而是避免 `proc` 被复用时保留旧进程状态。

### fork

在 `fork()` 中复制父进程的 mmap 状态：

```c
np->mmapaddr = p->mmapaddr;

for(int i = 0; i < MAXVMA; i++){
  np->vmas[i] = p->vmas[i];
  if(np->vmas[i].used)
    filedup(np->vmas[i].file);
}
```

含义：

```text
子进程继承父进程的 VMA 布局。
子进程之后访问 mmap 地址时，可以重新 page fault 并从文件加载。
filedup 保证父子进程各自释放 VMA 时不会提前释放同一个 file。
```

### exit

在 `exit()` 中释放所有有效 VMA：

```c
for(int i = 0; i < MAXVMA; i++){
  if(p->vmas[i].used == 1){
    domunmap(p->vmas[i].addr, p->vmas[i].length);
  }
}
```

这个步骤很重要。否则 mmap 高地址页不在 `[0, p->sz)` 中，`proc_freepagetable(p->pagetable, p->sz)` 不会释放它们。最后 `freewalk()` 会发现页表中残留 leaf PTE，导致：

```text
panic: freewalk: leaf
```

## 七、sys_mmap 实现

修改文件：`kernel/sysfile.c`

当前 `sys_mmap()` 完成这些工作：

```text
1. 读取用户参数。
2. 校验 length、offset、prot、flags。
3. 通过 fd 找到 struct file。
4. 检查文件权限。
5. 找一个空闲 VMA。
6. 从 mmapaddr 向下分配一段虚拟地址。
7. 填充 VMA。
8. filedup 保存文件引用。
9. 返回映射起始地址。
```

关键检查：

```c
if(length <= 0)
  return -1;

if(offset < 0 || offset % PGSIZE != 0)
  return -1;

if((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0)
  return -1;

if(flags != MAP_SHARED && flags != MAP_PRIVATE)
  return -1;

if(!f->readable)
  return -1;

if((prot & PROT_WRITE) && flags == MAP_SHARED && !f->writable)
  return -1;
```

这里区分了：

```text
MAP_SHARED | PROT_WRITE 需要 fd 可写。
MAP_PRIVATE | PROT_WRITE 不要求 fd 可写，因为修改不写回文件。
```

## 八、page fault 懒加载

修改文件：`kernel/trap.c`

在 `usertrap()` 中处理：

```text
scause == 13  load page fault
scause == 15  store/AMO page fault
```

当前流程：

```text
1. va = PGROUNDDOWN(r_stval())。
2. 在 p->vmas[] 中查找包含 va 的 VMA。
3. 如果找不到，kill 当前进程。
4. kalloc 分配一页物理内存。
5. memset 清零。
6. 根据 fileoff = v->offset + (va - v->addr) 计算文件偏移。
7. readi 从文件读取一页到 mem。
8. 根据 prot 生成 PTE 权限。
9. mappages 建立页表映射。
10. 返回用户态，重新执行 fault 指令。
```

关键代码思路：

```c
uint64 va = PGROUNDDOWN(r_stval());
uint64 fileoff = v->offset + (va - v->addr);

char *mem = kalloc();
memset(mem, 0, PGSIZE);

ilock(v->file->ip);
readi(v->file->ip, 0, (uint64)mem, fileoff, PGSIZE);
iunlock(v->file->ip);

mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm);
```

曾经遇到的问题：

```text
readi 第三个参数需要 uint64，不能直接传 char *。
mappages 参数顺序是 va, size, pa, perm，不能写反。
fault 地址必须 PGROUNDDOWN 后再映射。
```

## 九、domunmap / munmap

当前 `domunmap()` 放在 `kernel/proc.c`，用于：

```text
sys_munmap 调用
exit 清理 VMA 调用
```

它做的事情：

```text
1. 根据 addr/length 找到对应 VMA。
2. 只支持从 VMA 头部、尾部或全部解除映射。
3. 遍历解除范围内的每一页。
4. 对已经实际映射的页：
   - MAP_SHARED 时写回文件。
   - uvmunmap 解除页表映射并释放物理页。
5. 根据解除位置调整 VMA。
6. 如果整个 VMA 被解除，fileclose 并清空 VMA。
```

重要点：

```text
mmap 是 lazy 的，某些页可能从未 fault 过。
munmap 不能直接对所有页调用 uvmunmap。
必须先用 walkaddr 判断页面是否实际映射。
```

MAP_SHARED 写回逻辑：

```c
begin_op();
ilock(v->file->ip);
writei(v->file->ip, 0, pa, fileoff, PGSIZE);
iunlock(v->file->ip);
end_op();
```

这里 `writei` 的 `user_src` 参数是 `0`，因为 `pa` 是内核可访问的物理内存地址，不是用户虚拟地址。

## 十、遇到并修复的问题

### 1. mmap 返回类型错误

现象：

```text
initialization of 'char *' from 'int' makes pointer from integer without a cast
```

原因：

```text
user/user.h 中 mmap 声明成 int。
```

修复：

```c
void* mmap(void *addr, int length, int prot, int flags, int fd, int offset);
```

### 2. file.h 不完整类型错误

现象：

```text
field 'lock' has incomplete type
'NDIRECT' undeclared
```

原因：

```text
在 proc.c/trap.c 中 include file.h 前，没有 include sleeplock.h 和 fs.h。
```

修复：

```c
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
```

### 3. mappages 参数顺序错误

错误写法：

```c
mappages(p->pagetable, va, mem, PGSIZE, perm)
```

正确写法：

```c
mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm)
```

函数原型：

```c
int mappages(pagetable_t, uint64 va, uint64 size, uint64 pa, int perm);
```

### 4. munmap 不能直接 unmap 未映射页

由于 mmap 是懒加载，某些 VMA 页可能还没有实际 PTE。

处理方式：

```c
uint64 pa = walkaddr(p->pagetable, va);
if(pa == 0)
  continue;
uvmunmap(p->pagetable, va, 1, 1);
```

### 5. exit 不释放 VMA 导致 freeproc panic

现象：

```text
panic: freewalk: leaf
```

原因：

```text
mmap 页位于高地址，不在 [0, p->sz) 中。
proc_freepagetable 只释放普通用户内存，没有释放 mmap 页。
```

修复：

```text
exit 中遍历 VMA，调用 domunmap。
```

## 十一、当前仍需重点验证的点

### 1. domunmap 使用 myproc

当前 `domunmap()` 内部使用：

```c
struct proc *p = myproc();
```

由于当前只在当前进程的 `sys_munmap()` 和 `exit()` 中调用，暂时可用。

更清晰的设计是改成：

```c
int vmaunmap(struct proc *p, uint64 addr, uint64 length);
```

这样函数不依赖 `myproc()`，结构更干净。

### 2. VMA 字段类型

当前：

```c
int offset;
int length;
```

实验里一般足够，但更稳妥可以用：

```c
uint64 offset;
uint64 length;
```

因为它们表示地址/长度。

### 3. MAP_SHARED 写回整页

当前 MAP_SHARED unmap 时按整页写回：

```c
writei(..., PGSIZE)
```

测试通常可以通过，但严格来说最后一页可能超过原映射 length 或文件大小。后续如果遇到边界问题，需要改成按剩余映射长度写回。

### 4. mmapaddr 不复用洞

当前 mmap 从高地址向下分配，munmap 后不回收地址洞。

实验通常不要求复杂地址复用，但大量 mmap/munmap 后可能浪费虚拟地址空间。

### 5. fork 只复制 VMA，不复制 mmap 物理页

这是合理的 lazy 策略。子进程访问时重新 page fault 并从文件读取。

但如果父进程有 `MAP_PRIVATE` 映射并已经修改过，严格语义下子进程应该看到 fork 时的内存内容。xv6 lab 测试通常不覆盖这个复杂场景。

## 十二、建议下一步测试

先单独跑：

```sh
mmaptest
```

然后跑：

```sh
usertests
```

最后：

```sh
make grade
```

如果 `make grade` 因 QEMU 创建本地 GDB socket 失败，需要在允许本地 socket 的环境中运行。

## 十三、整体流程总结

```text
mmap()
  -> sys_mmap()
  -> 检查参数和 fd 权限
  -> 记录 VMA
  -> filedup
  -> 返回虚拟地址

用户访问 mmap 地址
  -> page fault
  -> usertrap()
  -> 查找 VMA
  -> kalloc
  -> readi 从文件加载一页
  -> mappages 建立映射

munmap()
  -> sys_munmap()
  -> domunmap()
  -> MAP_SHARED 写回
  -> uvmunmap
  -> 调整或清空 VMA

fork()
  -> 复制 VMA
  -> filedup 每个 VMA 的 file

exit()
  -> domunmap 所有 VMA
  -> 关闭 fd
  -> 释放 cwd
  -> 进入 ZOMBIE
```

