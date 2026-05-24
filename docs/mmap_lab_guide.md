# mmap 实验解读

当前分支：`mmap`

当前实验：xv6-labs-2020 `mmap` lab

## 一、实验目标

`mmap` 实验要实现文件到用户虚拟地址空间的映射。

用户程序可以这样调用：

```c
char *p = mmap(0, PGSIZE * 2, PROT_READ, MAP_PRIVATE, fd, 0);
```

之后访问：

```c
p[0]
p[100]
p[PGSIZE]
```

这些看起来是普通内存访问，但实际内容来自 `fd` 对应文件。

核心目标是：

```text
mmap 记录一段文件映射区域
page fault 时懒加载文件内容
munmap 时解除映射
MAP_SHARED 需要把修改写回文件
fork/exit 要正确处理映射区域
```

## 二、需要实现的系统调用

需要实现：

```c
void *mmap(void *addr, int length, int prot, int flags, int fd, int offset);
int munmap(void *addr, int length);
```

`mmap()` 的参数含义：

```text
addr    用户建议映射地址，本实验通常传 0，表示让内核选择
length  映射长度
prot    映射权限，例如 PROT_READ、PROT_WRITE
flags   MAP_SHARED 或 MAP_PRIVATE
fd      被映射的文件描述符
offset  从文件哪个偏移开始映射
```

`munmap()` 的参数含义：

```text
addr    要解除映射的起始地址
length  要解除映射的长度
```

## 三、测试要求

本地测试在：

```text
user/mmaptest.c
grade-lab-mmap
```

主要测试点：

```text
1. mmap 文件后，映射内存能读到文件内容。
2. MAP_PRIVATE 修改内存后，不写回文件。
3. 只读 fd 不能创建 PROT_WRITE | MAP_SHARED 映射。
4. 读写 fd 可以创建 PROT_WRITE | MAP_SHARED 映射。
5. MAP_SHARED 修改后，munmap 时要写回文件。
6. munmap 支持部分解除映射。
7. 同时 mmap 两个文件不能互相影响。
8. fork 后子进程能访问继承来的 mmap 区域。
```

## 四、核心概念：VMA

`mmap()` 不应该立刻把所有文件内容读入内存。推荐做法是懒加载。

因此 `mmap()` 只记录一段虚拟地址区域的信息，这个记录通常叫：

```text
VMA = Virtual Memory Area
```

可以在 `kernel/proc.h` 中添加类似结构：

```c
#define NVMA 16

struct vma {
  int used;
  uint64 addr;
  uint64 length;
  int prot;
  int flags;
  struct file *file;
  uint64 offset;
};
```

然后在 `struct proc` 中添加：

```c
struct vma vmas[NVMA];
```

每个 VMA 表示：

```text
从 addr 开始的一段用户虚拟地址
长度为 length
映射到 file 的 offset 开始处
权限由 prot 决定
类型由 flags 决定
```

## 五、mmap 应该做什么

`sys_mmap()` 主要负责登记 VMA，不立即分配物理页。

推荐流程：

```text
1. 读取系统调用参数。
2. 根据 fd 找到 struct file。
3. 检查 length、prot、flags、offset 是否基本合法。
4. 检查权限是否合法。
5. 找一个空闲 VMA。
6. 选择一段未使用的用户虚拟地址。
7. 保存 VMA 信息。
8. 调用 filedup(f)。
9. 返回映射起始地址。
```

必须 `filedup(f)`，因为测试会在 `mmap()` 后关闭 fd：

```c
p = mmap(..., fd, 0);
close(fd);
_v1(p);
```

如果只保存 fd，不保存并引用 `struct file *`，`close(fd)` 后映射会失效。

## 六、page fault 时做什么

`mmap()` 返回后，对应虚拟地址还没有页表映射。

用户第一次访问映射地址时，会触发 page fault。

需要在 `kernel/trap.c:usertrap()` 中处理：

```text
scause == 13  load page fault
scause == 15  store/AMO page fault
```

处理流程：

```text
1. 用 r_stval() 得到 fault address。
2. 查找这个地址属于哪个 VMA。
3. 如果找不到，说明非法访问，kill 当前进程。
4. kalloc() 分配一个物理页。
5. 清零物理页。
6. 根据 VMA 的 file 和 offset 读取文件内容到物理页。
7. 根据 prot 设置 PTE 权限。
8. 用 mappages() 建立用户虚拟地址到物理页的映射。
9. 返回用户态，重新执行刚才触发 fault 的指令。
```

文件偏移计算：

```text
file offset = vma.offset + (fault_va - vma.addr)
```

页对齐时通常用：

```c
uint64 va = PGROUNDDOWN(r_stval());
```

PTE 权限大致对应：

```text
PROT_READ   -> PTE_R
PROT_WRITE  -> PTE_W
PROT_EXEC   -> PTE_X
用户页必须有 PTE_U
```

## 七、munmap 应该做什么

`munmap(addr, length)` 解除已有映射。

推荐流程：

```text
1. 找到包含 addr 的 VMA。
2. 对解除范围内已经实际映射的页面逐页处理。
3. 如果是 MAP_SHARED，并且页面被映射过，需要写回文件。
4. uvmunmap() 解除页表映射并释放物理页。
5. 调整 VMA 的 addr/length/offset。
6. 如果整个 VMA 都被解除，fileclose(vma->file)，清空 VMA。
```

测试主要要求支持：

```text
从 VMA 开头解除一部分
从 VMA 末尾解除一部分
解除整个 VMA
```

一般不要求支持从中间挖洞。

注意：`munmap()` 可能解除尚未 page fault 过的页面。  
这些页面没有实际 PTE，不能因为它们没映射就 panic。

## 八、MAP_SHARED 和 MAP_PRIVATE

`MAP_PRIVATE`：

```text
映射内容来自文件。
用户修改映射内存后，不写回文件。
```

测试中：

```c
p = mmap(..., PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
p[i] = 'Z';
munmap(p, ...);
```

文件内容不应该被改成 `Z`。

`MAP_SHARED`：

```text
映射内容来自文件。
用户修改映射内存后，munmap/exit 时需要写回文件。
```

测试中会检查：

```text
写 p[i] = 'Z'
munmap
重新 read 文件
文件内容应该包含 'Z'
```

## 九、权限检查

这个必须处理：

```c
fd = open(f, O_RDONLY);
p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

应该失败。

原因：

```text
MAP_SHARED | PROT_WRITE 表示修改可能写回文件。
如果 fd 不是 writable，就不能允许。
```

但这个应该允许：

```c
fd = open(f, O_RDONLY);
p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
```

原因：

```text
MAP_PRIVATE 的修改不写回文件。
```

## 十、fork 需要做什么

`fork()` 后，子进程应该继承父进程的 VMA。

可以在 `kernel/proc.c:fork()` 中复制 VMA 数组：

```text
child.vmas[i] = parent.vmas[i]
```

对每个有效 VMA，要调用：

```c
filedup(vma->file);
```

可以不复制已经 fault 进来的 mmap 物理页，让子进程之后访问时重新 page fault 并从文件加载。

测试要求：

```text
父进程 mmap 文件
fork
子进程访问 mmap 区域
应该能读到正确内容
```

## 十一、exit 需要做什么

进程退出时，需要释放所有 VMA。

可以在 `exit()` 或相关释放路径中，对每个有效 VMA 执行类似 `munmap()` 的逻辑：

```text
1. 如果 MAP_SHARED，写回已映射页面。
2. 解除页表映射。
3. 释放物理页。
4. fileclose(vma->file)。
5. 清空 VMA。
```

否则会出现：

```text
文件引用泄漏
物理页泄漏
页表残留映射
```

## 十二、建议修改文件

通常需要修改：

```text
kernel/syscall.h     添加 SYS_mmap / SYS_munmap
kernel/syscall.c     注册 sys_mmap / sys_munmap
user/user.h          添加 mmap / munmap 声明
user/usys.pl         添加 syscall stub
kernel/proc.h        添加 struct vma 和 proc.vmas[]
kernel/sysfile.c     实现 sys_mmap / sys_munmap
kernel/trap.c        处理 mmap page fault
kernel/proc.c        fork/exit/freeproc 处理 VMA
kernel/vm.c          可能需要辅助函数，避免 unmap 未映射页时 panic
```

## 十三、推荐实现顺序

建议按这个顺序做：

```text
1. 读 user/mmaptest.c，明确测试行为。
2. 添加 mmap/munmap 系统调用入口，让程序能链接。
3. 在 proc.h 中添加 VMA 结构和数组。
4. 实现 sys_mmap，只记录 VMA，不加载页面。
5. 修改 usertrap，处理 mmap 区域 page fault。
6. 实现 sys_munmap，解除映射。
7. 支持 MAP_SHARED 写回。
8. 修改 fork，复制 VMA 并 filedup。
9. 修改 exit，释放 VMA。
10. 跑 mmaptest。
11. 跑 usertests 和 make grade。
```

## 十四、整体调用链

`mmap()` 调用链：

```text
user mmap(...)
  -> usys.S syscall stub
  -> ecall
  -> syscall()
  -> sys_mmap()
  -> 记录 VMA
  -> 返回起始虚拟地址
```

访问映射地址：

```text
user read/write mapped address
  -> page fault
  -> usertrap()
  -> 查找 VMA
  -> kalloc
  -> readi 从文件读内容
  -> mappages 建立页表映射
  -> 返回用户态
```

`munmap()` 调用链：

```text
user munmap(addr, length)
  -> syscall()
  -> sys_munmap()
  -> 必要时写回 MAP_SHARED 页面
  -> uvmunmap 解除映射
  -> 调整或清空 VMA
  -> fileclose
```

## 十五、一句话总结

`mmap` 实验的本质是：

```text
给进程增加 VMA 表；
mmap 只登记文件映射；
page fault 时按需从文件加载页面；
munmap/exit 时释放映射；
MAP_SHARED 在解除映射时写回文件；
fork 时继承 VMA。
```

