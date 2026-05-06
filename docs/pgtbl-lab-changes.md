# pgtbl 实验相关改动整理

本文档整理当前仓库中围绕 xv6 pgtbl 实验做过的主要改动。核心目标有三个：

1. 打印页表结构，即 `vmprint`。
2. 为每个进程维护独立的内核页表 `p->kpagetable`。
3. 把用户页映射同步到进程内核页表，使 `copyin/copyinstr` 可以使用新版实现。

## 一、辅助环境改动

### `gradelib.py`

修改点：

```python
try:
    from shlex import quote
except ImportError:
    from pipes import quote
```

原因：

原始代码使用 `pipes.quote`，但较新的 Python 版本中 `pipes` 已被移除。改成优先使用 `shlex.quote`，用于修复 `make grade` 在新 Python 环境下运行失败的问题。

### `kernel/entry.S`

修改点：

```asm
.globl _entry
_entry:
```

原因：

显式导出 `_entry` 符号，避免链接器找不到入口符号或产生入口相关警告。

## 二、vmprint 页表打印

相关文件：

```text
kernel/vm.c
kernel/defs.h
kernel/exec.c
```

### `vmprint`

当前新增了：

```c
void vmprint(pagetable_t pagetable);
static void vmprint_walk(pagetable_t pagetable, int depth);
```

作用：

递归打印三级页表中所有有效 PTE。打印格式大致是：

```text
page table 0x...
 ..0: pte ... pa ...
 .. ..0: pte ... pa ...
```

核心判断：

```c
if((pte & (PTE_R | PTE_W | PTE_X)) == 0)
```

含义：

如果一个 PTE 有 `PTE_V`，但没有 `PTE_R/PTE_W/PTE_X`，它不是 leaf PTE，而是指向下一级页表，所以递归打印。

### `exec.c` 中打印第一个进程页表

当前在 `exec()` 成功提交新用户页表后有：

```c
if(p->pid==1)
  vmprint(p->pagetable);
```

作用：

实验第一题要求在第一个进程执行 `exec` 后打印页表。

## 三、每进程内核页表

相关文件：

```text
kernel/proc.h
kernel/proc.c
kernel/vm.c
kernel/defs.h
```

### `struct proc` 增加 `kpagetable`

文件：

```text
kernel/proc.h
```

新增字段：

```c
pagetable_t kpagetable;
```

作用：

每个进程拥有自己的内核页表。调度器切换到某个进程时，不再一直使用全局 `kernel_pagetable`，而是使用当前进程的 `p->kpagetable`。

### `ukvminit`

文件：

```text
kernel/vm.c
```

新增函数：

```c
pagetable_t ukvminit(void);
```

作用：

创建一张新的进程内核页表，并映射内核运行所需的公共区域：

```text
UART0
VIRTIO0
CLINT
PLIC
kernel text
kernel data
TRAMPOLINE
```

注意：

进程自己的 kernel stack 不在 `ukvminit()` 中统一映射，而是在 `allocproc()` 中按进程单独映射。

### `ukvmmap`

文件：

```text
kernel/vm.c
```

新增函数：

```c
void ukvmmap(pagetable_t kpagetable, uint64 va, uint64 pa, uint64 sz, int perm);
```

作用：

类似原来的 `kvmmap()`，但目标页表不是全局 `kernel_pagetable`，而是某个进程自己的 `kpagetable`。

### `allocproc`

文件：

```text
kernel/proc.c
```

新增逻辑：

```c
p->kpagetable = ukvminit();
uint64 va = KSTACK((int)(p - proc));
pte_t pa = kvmpa(va);
ukvmmap(p->kpagetable, va, pa, PGSIZE, PTE_R | PTE_W);
p->kstack = va;
```

作用：

1. 为新进程创建独立内核页表。
2. 把该进程自己的 kernel stack 映射进这张内核页表。

这里的 `kvmpa(va)` 是因为原始 xv6 已经在 `procinit()` 里为每个进程分配并映射了 kernel stack。这里复用已有物理页，把它映射到进程自己的内核页表里。

### `scheduler`

文件：

```text
kernel/proc.c
```

新增逻辑：

```c
w_satp(MAKE_SATP(p->kpagetable));
sfence_vma();
swtch(&c->context, &p->context);
kvminithart();
```

含义：

切换到进程运行前，CPU 使用该进程自己的内核页表。

当进程切回调度器后，恢复全局内核页表。

## 四、释放进程内核页表

相关文件：

```text
kernel/proc.c
kernel/vm.c
```

### `ukvmunmap`

文件：

```text
kernel/vm.c
```

新增函数：

```c
void ukvmunmap(pagetable_t pagetable, uint64 va, uint64 npages);
```

作用：

从进程内核页表中删除 leaf PTE，但不释放映射到的物理页。

关键行为：

```text
pte == 0:
  continue

PTE_V == 0:
  continue

PTE_FLAGS(*pte) == PTE_V:
  panic

leaf PTE:
  *pte = 0
```

原因：

`p->kpagetable` 只是借用内核页、设备页、用户页的映射关系，不拥有这些物理页，所以不能释放物理页。

### `ufreewalk`

文件：

```text
kernel/vm.c
```

新增函数：

```c
void ufreewalk(pagetable_t pagetable);
```

作用：

释放进程内核页表使用的页表页。

注意：

调用它之前必须先用 `ukvmunmap()` 清掉所有 leaf PTE。否则页表里还残留普通映射，释放页表页时会出错或留下不清晰的状态。

### `uprockvmfree`

文件：

```text
kernel/vm.c
```

新增函数：

```c
void uprockvmfree(struct proc *p);
```

作用：

释放 `p->kpagetable` 中的内核相关映射，包括：

```text
kernel stack
TRAMPOLINE
kernel text/data
PLIC
CLINT
VIRTIO0
UART0
```

然后调用：

```c
ufreewalk(kpagetable);
```

### `freeproc`

文件：

```text
kernel/proc.c
```

当前新增逻辑：

```c
int sz = p->sz;

if(p->kpagetable){
  ukvmunmap(p->kpagetable, 0, PGROUNDUP(sz)/PGSIZE);
  uprockvmfree(p);
  p->kpagetable = 0;
}
```

作用：

释放进程时，先删除 `p->kpagetable` 中同步过来的用户映射，再释放进程内核页表自己的页表页。

注意：

这里的 `ukvmunmap(p->kpagetable, 0, PGROUNDUP(sz)/PGSIZE)` 不能释放用户物理页。用户物理页仍由 `proc_freepagetable(p->pagetable, p->sz)` 负责释放。

## 五、同步用户映射到进程内核页表

相关文件：

```text
kernel/vm.c
kernel/proc.c
kernel/exec.c
kernel/defs.h
```

### `ukvmcopyrange`

文件：

```text
kernel/vm.c
```

新增函数：

```c
int ukvmcopyrange(pagetable_t upagetable,
                  pagetable_t kpagetable,
                  uint64 oldsz,
                  uint64 newsz);
```

作用：

把用户页表 `upagetable` 中某个虚拟地址范围内的用户页映射，同步到进程内核页表 `kpagetable`。

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

为什么要清掉 `PTE_U`：

用户页表中的用户页带有 `PTE_U`。同步到内核页表后，需要去掉 `PTE_U`，表示这张映射不是给用户态直接使用的。

为什么跳过 `PTE_U == 0` 的页：

`exec()` 会通过 `uvmclear()` 创建 guard page。guard page 在页表里存在，但没有 `PTE_U`，不能当作普通用户页同步。

当前限制：

```c
if(newsz > CLINT)
  return -1;
```

原因：

当前 `ukvminit()` 把 `CLINT` 映射进了每进程内核页表。用户地址如果增长到 `CLINT`，会和已有设备映射冲突，导致 `mappages()` 触发 `remap` panic。

## 六、各生命周期路径中的同步

### `userinit`

文件：

```text
kernel/proc.c
```

新增：

```c
ukvmcopyrange(p->pagetable, p->kpagetable, 0, PGSIZE);
```

作用：

第一个用户进程由 `userinit()` 创建，它没有父进程，所以需要手动把 `uvminit()` 创建的第 0 页同步进 `p->kpagetable`。

### `fork`

文件：

```text
kernel/proc.c
```

新增：

```c
if(ukvmcopyrange(np->pagetable, np->kpagetable, 0, p->sz) < 0){
  freeproc(np);
  release(&np->lock);
  return -1;
}
```

作用：

`fork()` 中 `uvmcopy()` 已经复制了父进程用户页表到子进程用户页表。之后需要把子进程自己的用户映射同步到子进程自己的 `np->kpagetable`。

这里应该从 `0` 开始同步，而不是从 `PGSIZE` 开始，否则会漏掉用户第 0 页。

### `growproc`

文件：

```text
kernel/proc.c
```

当前逻辑：

```c
if(n > 0){
  sz = uvmalloc(p->pagetable, sz, sz + n);
  ukvmcopyrange(p->pagetable, p->kpagetable, oldsz, sz);
} else if(n < 0){
  ukvmunmap(p->kpagetable,
            PGROUNDUP(sz+n),
            (PGROUNDUP(sz)-PGROUNDUP(sz+n))/PGSIZE);
  sz = uvmdealloc(p->pagetable, sz, sz + n);
}
```

作用：

`sbrk()` 最终会调用 `growproc()`。因此用户地址空间增长或缩小时，必须同步维护 `p->kpagetable`。

增长时：

```text
1. uvmalloc() 先在用户页表中建立新映射。
2. ukvmcopyrange() 再把新增范围同步到 p->kpagetable。
```

缩小时：

```text
1. ukvmunmap() 先删除 p->kpagetable 中的借用映射。
2. uvmdealloc() 再删除用户页表映射并释放用户物理页。
```

### `exec`

文件：

```text
kernel/exec.c
```

新增逻辑：

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

作用：

`exec()` 会丢弃旧用户页表并创建新的用户页表。因此 `p->kpagetable` 中旧的用户映射必须先删除，再同步新用户页表中的用户映射。

## 七、切换 `copyin/copyinstr` 到新版

相关文件：

```text
kernel/vm.c
kernel/vmcopyin.c
kernel/defs.h
```

### `defs.h` 中新增声明

```c
int copyin_new(pagetable_t, char *, uint64, uint64);
int copyinstr_new(pagetable_t, char *, uint64, uint64);
```

### `copyin`

文件：

```text
kernel/vm.c
```

当前改为：

```c
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  return copyin_new(pagetable, dst, srcva, len);
}
```

### `copyinstr`

当前改为：

```c
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  return copyinstr_new(pagetable, dst, srcva, max);
}
```

含义：

旧版 `copyin/copyinstr` 会通过 `walkaddr()` 手动查用户页表，把用户虚拟地址转换为物理地址。

新版 `copyin_new/copyinstr_new` 直接使用用户虚拟地址访问内存。这要求当前 CPU 正在使用的内核页表，也就是 `p->kpagetable`，必须包含对应用户虚拟地址的映射。

## 八、当前调试信息和风险点

### `mappages()` 中有调试打印

当前 `mappages()` 中加入了：

```c
printf("remap va=%p pa=%p oldpte=%p oldpa=%p perm=%p\n",
       a, pa, *pte, PTE2PA(*pte), perm);
panic("remap");
```

作用：

当发生重复映射时，打印冲突的虚拟地址、物理地址和旧 PTE，方便判断是用户映射重复、设备地址冲突，还是 kernel stack/trampoline 冲突。

注意：

这是调试信息。最终提交实验时可以保留，也可以去掉；如果测试严格比较 panic 输出，建议去掉。

### `CLINT` 地址冲突

曾经出现：

```text
va=0x0000000002000000
oldpa=0x0000000002000000
```

这个地址是 `CLINT`。说明用户页同步到 `p->kpagetable` 时，撞到了已经存在的设备映射。

当前通过：

```c
if(newsz > CLINT)
  return -1;
```

限制用户地址空间不能增长到 `CLINT` 以上。

### 仍需重点检查

1. `ukvminit()` 当前没有检查 `kalloc()` 返回值是否为 `0`，如果内存不足，`memset(0, ...)` 会崩。
2. `growproc()` 中 `ukvmcopyrange()` 失败时当前是 `panic`，更稳妥的做法是回滚刚刚 `uvmalloc()` 的用户页并返回 `-1`。
3. `exec()` 中 `ukvmcopyrange()` 失败时当前是 `panic`，实验中通常可以先接受，但更完整的实现应考虑失败恢复。
4. `ufreewalk()` 当前会无条件把 `pagetable[i] = 0`，即使遇到 leaf PTE 也不会 panic。这能避免崩溃，但会掩盖“释放前没有 unmap 干净”的 bug。更严格的写法应该在遇到 leaf PTE 时 panic。
5. `ukvmcopy(struct proc *)` 目前看起来已经被 `ukvmcopyrange()` 取代，如果不再使用，可以后续清理。
6. 需要跑 `make grade` 或至少 `usertests` 验证完整正确性。

## 九、整体数据流总结

每个进程现在有两张页表：

```text
p->pagetable:
  用户页表，用户态运行时使用。

p->kpagetable:
  进程内核页表，进程进入内核后使用。
```

切换逻辑：

```text
scheduler()
  -> w_satp(MAKE_SATP(p->kpagetable))
  -> 运行该进程的内核代码
  -> swtch 返回调度器
  -> kvminithart() 恢复全局 kernel_pagetable
```

同步不变量：

```text
p->pagetable 中存在的用户页映射，
需要同步到 p->kpagetable 中。

但同步到 p->kpagetable 时，
必须清掉 PTE_U。
```

主要维护点：

```text
userinit:
  第一个进程手动同步 [0, PGSIZE)

fork:
  子进程复制用户页表后，同步 [0, np->sz)

growproc:
  sbrk 增长时同步新增范围
  sbrk 缩小时删除对应范围

exec:
  删除旧用户映射
  同步新用户映射

freeproc:
  删除 p->kpagetable 中用户映射
  释放进程内核页表
```

完成这些维护后，`copyin_new/copyinstr_new` 才能依赖 `p->kpagetable` 直接访问用户虚拟地址。
