# cow 分支实验过程总结

本文档总结当前 `cow` 分支完成的工作、涉及的代码路径、调试过程中遇到的问题、已采用的解决方法，以及当前仍需注意的风险点。

当前分支：

```sh
git branch --show-current
# cow
```

当前主要改动文件：

```text
kernel/riscv.h
kernel/kalloc.c
kernel/defs.h
kernel/vm.c
kernel/trap.c
Makefile
gradelib.py
user/usertests.c
time.txt
```

其中 `user/usertests.c` 目前包含调试用打印，正式提交前应删除。

## 一、实验目标

COW，全称 copy-on-write，目标是优化 `fork()`。

原始 xv6 的 `fork()` 会调用 `uvmcopy()`，把父进程的所有用户物理页逐页复制给子进程：

```text
父进程用户页 A
fork()
子进程得到新物理页 B
memmove(B, A, PGSIZE)
```

这样实现简单，但代价很高。如果子进程马上 `exec()`，这些复制出来的页面马上就会被丢弃，复制工作完全浪费。

COW 的目标是：

```text
fork 时不复制物理页
父子进程共享同一批物理页
共享页暂时取消 PTE_W
只有某个进程真正写共享页时，才分配新页并复制
```

也就是：

```text
fork()
  -> 父子共享物理页
  -> PTE 标记为 COW

写 COW 页
  -> store page fault
  -> 内核分配新物理页
  -> 复制旧页内容
  -> 当前进程 PTE 指向新页
  -> 恢复 PTE_W
```

## 二、页表标志位：PTE_COW

修改文件：

```text
kernel/riscv.h
```

新增：

```c
#define PTE_COW (1L << 8)
```

RISC-V PTE 中有若干软件可用位，硬件不会解释这些位。COW 实验用其中一个软件位记录：

```text
这个 PTE 原来是可写页
现在因为 fork 被改成只读
如果后续发生写 fault，需要执行 COW 分配
```

注意：`PTE_COW` 不是硬件权限位。硬件真正阻止写入的是 `PTE_W` 被清掉。

## 三、物理页引用计数

修改文件：

```text
kernel/kalloc.c
kernel/defs.h
```

### 1. 新增 refcnt 数组

```c
int refcnt[PHYSTOP/PGSIZE];
```

含义：

```text
refcnt[pa / PGSIZE] = 该物理页当前被多少个 PTE 引用
```

COW 需要引用计数，因为多个进程可能同时映射同一个物理页。只有引用计数减到 0，物理页才能真正回到 freelist。

### 2. freerange 初始化

当前实现中，`freerange()` 在启动阶段把空闲物理页加入 freelist：

```c
refcnt[(uint64)p/PGSIZE] = 1;
kfree(p);
```

这样做是为了配合修改后的 `kfree()`。

修改后的 `kfree()` 会先检查引用计数：

```c
if(refcnt[(uint64)r/PGSIZE] < 1)
  panic("ref cnt");
```

如果启动阶段直接 `kfree(p)`，引用计数初始为 0，就会触发 `panic: ref cnt`。因此在 `freerange()` 中先设为 1，再调用 `kfree()`，使其减到 0 并进入 freelist。

这是之前遇到“启动时引用计数小于 0 / ref cnt panic”的原因和解决方法。

### 3. kalloc

当前 `kalloc()` 做了三件事：

```text
从 freelist 取出一页
填充 junk 值 5
设置 refcnt[pa/PGSIZE] = 1
```

含义是：刚分配出来的物理页默认只有一个所有者。

### 4. kfree

当前 `kfree()` 不再总是直接释放物理页，而是：

```text
检查 pa 合法性
检查 refcnt >= 1
refcnt--
如果 refcnt == 0，才 memset 并放回 freelist
如果 refcnt > 0，不释放
```

这保证共享页不会被某一个进程释放后立刻回收。

### 5. krefinc

新增函数：

```c
void krefinc(uint64 pa)
```

作用：

```text
当一个已有物理页被新的 PTE 共享映射时，引用计数 +1
```

它在 `uvmcopy()` 中使用。

## 四、fork 路径：uvmcopy 改为 COW

修改文件：

```text
kernel/vm.c
```

原始 `uvmcopy()` 做法：

```text
遍历父进程用户页
kalloc 新物理页
memmove 复制内容
mappages 映射到子进程
```

当前 COW 做法：

```text
遍历父进程用户页
如果 PTE_W 为 1：
  设置 PTE_COW
  清除 PTE_W
子进程映射同一个物理页
krefinc(pa)
sfence_vma()
```

核心代码逻辑：

```c
if(*pte & PTE_W){
  *pte |= PTE_COW;
  *pte &= ~PTE_W;
}

flags = PTE_FLAGS(*pte);

if(mappages(new, i, PGSIZE, pa, flags) != 0){
  goto err;
}

krefinc(pa);
```

这样 fork 后：

```text
父进程 PTE -> 同一个 pa，只读，COW
子进程 PTE -> 同一个 pa，只读，COW
refcnt[pa]  += 1
```

为什么清除 `PTE_W`：

```text
如果不清除 PTE_W，用户写入不会 fault
内核就没有机会执行 COW
父子进程会直接互相修改共享物理页
```

为什么要 `sfence_vma()`：

```text
父进程原来可能已经在 TLB 中缓存了可写权限
修改 PTE 后，需要刷新 TLB
否则 CPU 可能继续使用旧的可写翻译
```

## 五、写 COW 页：usertrap 中处理 store page fault

修改文件：

```text
kernel/trap.c
```

COW 写入会触发 RISC-V 的 store page fault。

在 xv6/RISC-V 中：

```text
scause == 15 表示 store/AMO page fault
stval 保存 fault 的虚拟地址
```

当前在 `usertrap()` 中增加：

```c
else if(r_scause() == 15){
  uint64 va = PGROUNDDOWN(r_stval());
  if(cowalloc(p, va) < 0)
    p->killed = 1;
}
```

处理逻辑：

```text
用户态写某个 COW 页
硬件发现 PTE_W=0
触发 store page fault
进入 usertrap
调用 cowalloc
成功则返回用户态重试原指令
失败则杀死进程
```

## 六、cowalloc 的作用

修改文件：

```text
kernel/trap.c
```

当前实现：

```c
uint64 cowalloc(struct proc *p, uint64 va)
```

它负责把当前进程的某个 COW 虚拟页变成独占可写页。

主要步骤：

```text
1. 检查 va < p->sz
2. walk 找到 PTE
3. 检查 PTE_V
4. 检查 PTE_U
5. 检查 PTE_COW
6. kalloc 新页
7. memmove 复制旧物理页内容
8. 修改当前 PTE 指向新物理页
9. 清除 PTE_COW，设置 PTE_W
10. kfree 旧物理页，使旧页 refcnt--
11. sfence_vma
```

关键点：

```text
kfree(oldpa) 不一定真的释放旧页
如果其他进程还共享旧页，只会 refcnt--
只有 refcnt == 0 才释放
```

当前 `cowalloc()` 失败时返回 `-1`，调用者将进程标记为 killed。

## 七、copyout 对 COW 的支持

修改文件：

```text
kernel/vm.c
```

COW 实验中还有一个容易漏掉的点：内核写用户地址时，也可能写到 COW 页。

典型场景：

```text
read(fd, user_buffer, n)
pipe read
wait(status)
exec 参数写用户栈
```

这些路径不是用户指令直接写内存，而是内核通过 `copyout()` 写用户页。如果目标页是 COW，只靠 `usertrap()` 不够，因为内核写用户页不会走用户态 store fault 的正常处理流程。

因此 `copyout()` 需要在写入前检查目标页是否带 `PTE_COW`。

当前实现思路：

```text
遍历 copyout 覆盖到的每一页
walk 找到 PTE
如果 PTE_COW：
  调用 cowalloc
之后再执行原本的 copyout 复制
```

当前代码已经加入这部分逻辑。

但是这里仍有一个需要注意的设计问题：

```c
cowalloc(myproc(), a)
```

`copyout()` 的参数本身是：

```c
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
```

理论上它应该处理传入的 `pagetable`。当前写法调用 `myproc()->pagetable`，在大多数当前进程场景下能工作，但设计上不够干净。如果未来 `copyout()` 写的不是 `myproc()->pagetable`，就可能出错。

更稳妥的方案是把 `cowalloc()` 改成接收 `pagetable_t`：

```c
int cowalloc(pagetable_t pagetable, uint64 sz, uint64 va)
```

这样 `copyout()` 可以严格处理传入的页表。

## 八、遇到的问题与解决过程

### 1. 启动时 panic: ref cnt

现象：

```text
xv6 启动时发生引用计数小于 0 或 ref cnt panic
```

原因：

```text
kfree() 被改成需要 refcnt >= 1
但 kinit/freerange 初始化空闲页时，refcnt 还是 0
直接 kfree 会触发检查失败
```

解决：

```c
refcnt[(uint64)p/PGSIZE] = 1;
kfree(p);
```

也就是在 `freerange()` 里先人为设为 1，再让 `kfree()` 减到 0 并放入 freelist。

### 2. COW fault 中 PTE 更新错误

曾经讨论过类似错误写法：

```c
uint64 flags = PTE_FLAGS(*pte) | PTE_W & ~PTE_COW;
*pte = PTE2PA(pa) | flags;
```

问题有两个：

```text
运算符优先级错误
PTE2PA(pa) 用法错误
```

正确思路：

```c
uint64 flags = (PTE_FLAGS(*pte) & ~PTE_COW) | PTE_W;
*pte = PA2PTE((uint64)mem) | flags;
```

解释：

```text
PTE_FLAGS(*pte) 取旧权限
& ~PTE_COW 清除 COW 标记
| PTE_W 恢复可写
PA2PTE(mem) 把新物理地址编码成 PTE 格式
```

### 3. 什么时候 panic，什么时候 killed

COW fault 发生在用户态时，不应该因为用户传了坏地址就让内核 panic。

原则：

```text
内核自身不变量被破坏：panic
用户进程传非法地址或写非法页：p->killed = 1
```

例如：

```text
用户写 va >= p->sz
PTE 不存在
PTE 不是用户页
PTE 不是 COW 页
kalloc 失败
```

这些更适合返回错误并杀死当前用户进程，而不是 panic 整个内核。

### 4. copyout 没有处理 COW

问题：

```text
用户态写 COW 页会触发 usertrap
但内核 copyout 写 COW 页不一定走 usertrap 的 COW 分支
```

影响：

```text
pipe/read/wait 等系统调用可能写用户 COW 页失败
usertests 容易卡住或出现 kerneltrap
```

解决：

```text
在 copyout 写入前检查目标 PTE
如果有 PTE_COW，先执行 COW 分配
```

### 5. pgbug 触发 panic: walk

现象：

```text
usertests 的 pgbug 触发 panic: walk
```

判断：

```text
这不正常
pgbug 是回归测试，正确结果应是 test pgbug: OK
```

原因：

```text
用户传入异常地址
copyin/copyout/copyinstr/walkaddr 没有提前拦住 va >= MAXVA
非法 va 传入 walk()
walk() 中 if(va >= MAXVA) panic("walk")
```

正确原则：

```text
walk() 是底层函数，可以 panic
处理用户地址的上层函数必须先检查，非法则返回 -1
```

应重点检查：

```text
kernel/vm.c: walkaddr
kernel/vm.c: copyin
kernel/vm.c: copyout
kernel/vm.c: copyinstr
```

典型防护：

```c
if(va >= MAXVA)
  return -1;
```

并注意地址加法溢出：

```c
if(addr + len < addr)
  return -1;
```

### 6. usertests 卡在 test execout

现象：

```text
usertests starting
test execout:
后续长时间无输出
```

分析：

`execout` 本身就是一个很重的 OOM 测试。它会循环 fork 子进程，子进程不断：

```c
sbrk(4096)
*(char*)(a + 4096 - 1) = 1;
```

直到 `sbrk()` 返回 `-1`。

它主要压力测试：

```text
fork -> uvmcopy -> COW refcnt
sbrk -> growproc -> uvmalloc -> kalloc
exec -> 新页表创建与失败清理
exit -> uvmfree -> uvmunmap -> kfree
wait -> 回收子进程
```

通过打印 `a` 观察到：

```text
0x0000000007CC9000
0x0000000007CCA000
0x0000000007CCB000
0xFFFFFFFFFFFFFFFF
0x0000000000011000
0x0000000000012000
```

解释：

```text
前几行是一个子进程持续 sbrk 增长
0xffffffffffffffff 表示 sbrk 返回 -1，即 OOM
后面重新从低地址开始，是 execout 下一轮 fork 出来的新子进程
```

因此至少可以说明：

```text
sbrk 分配循环不是永远不返回
kalloc 能够返回 0
父进程至少进入了下一轮
```

如果后续仍卡住，应继续定位具体停在：

```text
sbrk(-4096)
exec("echo", args)
exit
wait
```

建议临时调试打印：

```c
printf("avail=%d pid=%d start\n", avail, getpid());
printf("avail=%d alloc done\n", avail);
printf("avail=%d before exec\n", avail);
printf("avail=%d exec returned\n", avail);
```

正式提交前必须删除这些打印。

### 7. make grade 的 Python 兼容问题

修改文件：

```text
gradelib.py
```

问题：

```text
Python 3.13 移除了 pipes 模块
grade 脚本 from pipes import quote 失败
```

解决：

```python
try:
    from shlex import quote
except ImportError:
    from pipes import quote
```

`shlex.quote` 是现代 Python 中推荐使用的替代方案。

### 8. GDB 调试环境配置

修改文件：

```text
Makefile
```

问题：

```text
系统中没有 riscv64-unknown-elf-gdb
但安装了 gdb-multiarch
GDB 还会拒绝自动加载当前目录 .gdbinit
```

解决：

```makefile
GDB ?= gdb-multiarch
GDBARGS = -iex "add-auto-load-safe-path $(CURDIR)/.gdbinit"
```

新增：

```makefile
gdb: $K/kernel .gdbinit
	$(GDB) $(GDBARGS) $K/kernel
```

现在调试方式：

终端 1：

```sh
make qemu-gdb
```

终端 2：

```sh
make gdb
```

常用断点：

```gdb
b panic
b usertrap
b cowalloc
b sys_sbrk
b growproc
b uvmalloc
b kalloc
b kfree
b uvmcopy
b exec
```

如果函数名断点失败，可以用文件行号：

```gdb
b kernel/sysproc.c:42
b kernel/proc.c:239
b kernel/kalloc.c:79
b kernel/kalloc.c:50
b kernel/exec.c:13
```

panic 后通常不能恢复运行，但可以查看现场：

```gdb
bt
info registers
p/x r_scause()
p/x r_stval()
p/x r_sepc()
```

## 九、重要调用链总结

### fork + COW

```text
user fork()
  -> ecall
  -> usertrap
  -> syscall
  -> sys_fork
  -> fork
  -> uvmcopy
      -> walk 父页表
      -> 清 PTE_W
      -> 设置 PTE_COW
      -> mappages 子页表到同一 pa
      -> krefinc(pa)
```

### 用户写 COW 页

```text
用户 store 指令
  -> PTE_W=0
  -> store page fault
  -> usertrap
  -> scause == 15
  -> cowalloc
      -> walk
      -> kalloc
      -> memmove
      -> 修改 PTE 到新 pa
      -> 清 PTE_COW
      -> 设置 PTE_W
      -> kfree(oldpa)
      -> sfence_vma
  -> 返回用户态
  -> 重试原 store 指令
```

### copyout 写 COW 页

```text
系统调用需要写用户缓冲区
  -> copyout
  -> walk 目标用户页
  -> 如果 PTE_COW：
      -> cowalloc
  -> walkaddr
  -> memmove 到用户物理页
```

### 释放用户地址空间

```text
exit / exec 旧地址空间释放
  -> proc_freepagetable
  -> uvmfree
  -> uvmunmap(..., do_free=1)
      -> kfree(pa)
          -> refcnt--
          -> refcnt == 0 才回 freelist
  -> freewalk
```

原则：

```text
uvmunmap 负责解除叶子 PTE，并根据 do_free 决定是否 kfree 物理页
freewalk 负责释放页表页本身
不要让 freewalk 看到 leaf PTE，否则会 panic("freewalk: leaf")
```

## 十、当前仍需注意的问题

### 1. usertests.c 中还有调试打印

当前改动：

```c
printf("%p\n", a);
```

位置：

```text
user/usertests.c:execout
```

这会污染 `usertests` 输出，正式跑 grade 前应删除。

### 2. copyout 的 COW 处理还可以更严谨

当前实现：

```c
cowalloc(myproc(), a)
```

建议后续改成：

```text
cowalloc 直接接收 pagetable
copyout 对传入的 pagetable 做 COW
```

这样设计更通用，也更符合 `copyout(pagetable, ...)` 的接口语义。

### 3. 地址边界和溢出检查需要继续补齐

重点检查：

```text
copyin
copyout
copyinstr
walkaddr
```

需要避免：

```text
va >= MAXVA
addr + len 溢出
PGROUNDDOWN 后仍把非法地址传入 walk
```

否则 `pgbug` 可能触发 `panic: walk`。

### 4. krefinc 缺少健壮性检查

当前：

```c
void krefinc(uint64 pa){
  acquire(&kmem.lock);
  refcnt[pa/PGSIZE]++;
  release(&kmem.lock);
}
```

后续可以考虑补充：

```text
pa 是否页对齐
pa 是否在合法物理内存范围内
refcnt 是否已经 >= 1
```

这样更容易发现错误引用。

### 5. kalloc/kfree 中 memset 在锁内

当前 `kalloc()` 和 `kfree()` 的 `memset` 在 `kmem.lock` 内执行。

这通常不会造成功能错误，但会扩大锁持有时间。后续可以参考原始 xv6，把耗时的 `memset` 尽量移到锁外，同时保持引用计数和 freelist 操作的原子性。

## 十一、建议的后续验证顺序

建议按下面顺序验证，不要直接一开始跑完整 `make grade`：

```sh
make
```

进入 xv6 后：

```sh
cowtest
```

如果 `cowtest` 通过，再跑单项 `usertests`。可以临时只保留某个测试，例如：

```text
execout
pgbug
copyin
copyout
copyinstr
```

最后再跑：

```sh
make grade
```

如果 `execout` 卡住，优先加临时打印定位：

```text
是否卡在 sbrk 增长
是否卡在 sbrk 缩小
是否卡在 exec
是否卡在 exit/wait
```

如果出现 page fault，优先用 GDB：

```gdb
b panic
b usertrap
b cowalloc
c
bt
p/x r_scause()
p/x r_stval()
p/x r_sepc()
```

## 十二、当前状态总结

当前分支已经完成了 COW 的主体结构：

```text
增加 PTE_COW
增加物理页引用计数
修改 kalloc/kfree
修改 uvmcopy 为共享映射
处理用户态 store page fault
在 copyout 中补充 COW 处理
修复 grade 脚本 Python 兼容问题
配置 gdb-multiarch 调试入口
```

当前重点风险仍在：

```text
copyout 的 COW 处理接口不够干净
非法用户地址可能传入 walk
execout/usertests 仍需完整验证
调试打印需要删除
```

这份分支目前不是“完全收尾状态”，而是“COW 主体已实现，正在通过 usertests 定位边界条件”的状态。
