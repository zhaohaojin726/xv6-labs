# xv6 系统调用流程详解

本文档说明 xv6-riscv 中一次系统调用如何从用户程序进入内核，再把返回值带回用户程序。

核心路径：

```text
user program
-> user/user.h 声明系统调用函数
-> user/usys.pl 生成 user/usys.S
-> ecall
-> kernel/trampoline.S:uservec
-> kernel/trap.c:usertrap()
-> kernel/syscall.c:syscall()
-> kernel/sysproc.c 或 kernel/sysfile.c 中的 sys_xxx()
-> kernel/trap.c:usertrapret()
-> kernel/trampoline.S:userret
-> sret
-> user program
```

## 1. 用户态看到的是普通函数

用户程序调用系统调用时，看起来像普通 C 函数。

例如：

```c
write(1, "hi\n", 3);
```

这些函数声明在 `user/user.h`：

```c
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int*);
int write(int, const void*, int);
int read(int, void*, int);
int exec(char*, char**);
char* sbrk(int);
int sleep(int);
int uptime(void);
```

但这些不是普通库函数实现。它们最终会通过 `ecall` 进入内核。

## 2. `user/usys.pl` 生成系统调用入口

文件：`user/usys.pl`

它是一个 Perl 脚本，用来生成 `user/usys.S`。

关键代码：

```perl
sub entry {
    my $name = shift;
    print ".global $name\n";
    print "${name}:\n";
    print " li a7, SYS_${name}\n";
    print " ecall\n";
    print " ret\n";
}
```

例如：

```perl
entry("write");
```

会生成类似汇编：

```asm
.global write
write:
  li a7, SYS_write
  ecall
  ret
```

含义：

```text
1. 把系统调用号放进 a7。
2. 执行 ecall，陷入内核。
3. 内核返回后，从 a0 取返回值，然后 ret 回用户代码。
```

## 3. 系统调用号

系统调用号定义在 `kernel/syscall.h`：

```c
#define SYS_fork    1
#define SYS_exit    2
#define SYS_wait    3
#define SYS_pipe    4
#define SYS_read    5
#define SYS_kill    6
#define SYS_exec    7
#define SYS_fstat   8
#define SYS_chdir   9
#define SYS_dup    10
#define SYS_getpid 11
#define SYS_sbrk   12
#define SYS_sleep  13
#define SYS_uptime 14
#define SYS_open   15
#define SYS_write  16
```

系统调用号放在寄存器 `a7`。

系统调用参数按 RISC-V 调用约定放在：

```text
a0, a1, a2, a3, a4, a5
```

返回值放在：

```text
a0
```

例如：

```c
write(fd, buf, n)
```

进入内核前寄存器大致是：

```text
a0 = fd
a1 = buf
a2 = n
a7 = SYS_write
```

## 4. `ecall` 做了什么

`ecall` 是 RISC-V 的环境调用指令。

用户态执行 `ecall` 后，硬件会：

```text
1. 从 user mode 进入 supervisor mode。
2. 把当前 PC 保存到 sepc。
3. 把 trap 原因保存到 scause。
4. 跳到 stvec 指向的地址。
```

在用户态运行时，`stvec` 被设置为：

```c
TRAMPOLINE + (uservec - trampoline)
```

所以系统调用发生后，CPU 会进入：

```text
kernel/trampoline.S:uservec
```

注意：刚进入 `uservec` 时，CPU 已经是 supervisor mode，但 `satp` 仍然是用户页表。

## 5. `trampoline.S:uservec`

文件：`kernel/trampoline.S`

`uservec` 是从用户态 trap 进内核的第一段代码。

它的任务：

```text
1. 保存用户寄存器到 TRAPFRAME。
2. 切换到当前进程的内核栈。
3. 切换到内核页表。
4. 跳到 usertrap()。
```

### 通过 `sscratch` 找到 TRAPFRAME

```asm
csrrw a0, sscratch, a0
```

`sscratch` 里提前保存了 `TRAPFRAME` 地址。交换之后：

```text
a0 = TRAPFRAME
sscratch = 用户原来的 a0
```

这样 `uservec` 可以用 `a0` 作为基址保存寄存器。

### 保存用户寄存器

```asm
sd ra, 40(a0)
sd sp, 48(a0)
sd gp, 56(a0)
...
sd a7, 168(a0)
...
```

这些偏移对应 `struct trapframe` 中各寄存器字段。

用户原来的 `a0` 被特殊处理：

```asm
csrr t0, sscratch
sd t0, 112(a0)
```

因为 `a0` 当前被用作 `TRAPFRAME` 地址，所以用户原来的 `a0` 需要从 `sscratch` 取回并保存。

### 切到内核栈

```asm
ld sp, 8(a0)
```

`8(a0)` 对应：

```c
p->trapframe->kernel_sp
```

这个值由 `usertrapret()` 提前设置：

```c
p->trapframe->kernel_sp = p->kstack + PGSIZE;
```

所以从这里开始，内核使用当前进程自己的 kernel stack。

### 恢复 hart id

```asm
ld tp, 32(a0)
```

`tp` 保存当前 CPU 的 hart id。xv6 的 `cpuid()` 依赖它。

### 取出 `usertrap()` 地址

```asm
ld t0, 16(a0)
```

这个字段是：

```c
p->trapframe->kernel_trap = (uint64)usertrap;
```

### 切到内核页表

```asm
ld t1, 0(a0)
csrw satp, t1
sfence.vma zero, zero
```

`0(a0)` 对应：

```c
p->trapframe->kernel_satp
```

它保存当前进程进入内核时应该使用的内核页表。

在当前 pgtbl 实验里，这里应当是当前进程自己的 `p->kpagetable` 对应的 `satp` 值。

### 跳到 `usertrap()`

```asm
jr t0
```

之后进入 C 代码：

```text
kernel/trap.c:usertrap()
```

## 6. `trap.c:usertrap()`

文件：`kernel/trap.c`

`usertrap()` 负责处理所有来自用户态的 trap，包括：

```text
系统调用
设备中断
异常
```

系统调用分支：

```c
if(r_scause() == 8){
  if(p->killed)
    exit(-1);

  p->trapframe->epc += 4;

  intr_on();

  syscall();
}
```

### 判断系统调用

```c
r_scause() == 8
```

在 RISC-V 中，用户态执行 `ecall` 会产生 environment call from U-mode，xv6 用 `scause == 8` 判断它是系统调用。

### 保存用户 PC

```c
p->trapframe->epc = r_sepc();
```

`sepc` 保存触发 trap 的用户指令地址。对于系统调用来说，就是 `ecall` 那条指令的地址。

### 跳过 `ecall`

```c
p->trapframe->epc += 4;
```

如果不加 4，返回用户态后会重新执行同一条 `ecall`，导致无限系统调用。

RISC-V 普通指令长度是 4 字节，所以加 4。

### 调用 `syscall()`

```c
syscall();
```

进入系统调用分发器。

## 7. `syscall.c:syscall()`

文件：`kernel/syscall.c`

核心代码：

```c
num = p->trapframe->a7;
if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
  p->trapframe->a0 = syscalls[num]();
} else {
  p->trapframe->a0 = -1;
}
```

### 读取系统调用号

```c
num = p->trapframe->a7;
```

用户态 wrapper 在执行 `ecall` 前把系统调用号放进 `a7`。`uservec` 保存了所有用户寄存器，所以内核可以从 `trapframe->a7` 读出来。

### 查表分发

```c
static uint64 (*syscalls[])(void) = {
  [SYS_fork]    sys_fork,
  [SYS_exit]    sys_exit,
  [SYS_wait]    sys_wait,
  [SYS_read]    sys_read,
  [SYS_write]   sys_write,
  ...
};
```

`syscalls[num]()` 调用真正的内核实现。

例如：

```text
SYS_write -> sys_write()
SYS_sbrk  -> sys_sbrk()
SYS_fork  -> sys_fork()
```

### 返回值写回 `trapframe->a0`

```c
p->trapframe->a0 = syscalls[num]();
```

RISC-V 调用约定用 `a0` 返回结果。内核把返回值写进 `trapframe->a0`，之后 `userret` 恢复用户寄存器时，用户程序就会在 `a0` 里看到返回值。

## 8. 参数如何取得

系统调用参数也来自 trapframe。

### `argraw()`

```c
static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
}
```

也就是说：

```text
第 0 个参数来自 a0
第 1 个参数来自 a1
第 2 个参数来自 a2
...
```

### `argint()`

```c
int
argint(int n, int *ip)
{
  *ip = argraw(n);
  return 0;
}
```

用于读取整数参数。

### `argaddr()`

```c
int
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
  return 0;
}
```

用于读取用户指针参数。它只取地址，不验证地址是否真的有效。

地址有效性由后面的 `copyin()`、`copyout()` 或文件读写路径检查。

### `argstr()`

```c
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  if(argaddr(n, &addr) < 0)
    return -1;
  return fetchstr(addr, buf, max);
}
```

用于读取字符串参数。它先取得用户虚拟地址，再通过 `fetchstr()` 和 `copyinstr()` 从用户地址空间复制字符串到内核缓冲区。

## 9. 示例：`write(1, buf, n)`

用户程序：

```c
write(1, buf, n);
```

用户态 wrapper：

```asm
write:
  li a7, SYS_write
  ecall
  ret
```

进入内核前：

```text
a0 = 1
a1 = buf
a2 = n
a7 = SYS_write
```

内核分发：

```text
usertrap()
-> syscall()
-> syscalls[SYS_write]()
-> sys_write()
```

`sys_write()` 在 `kernel/sysfile.c`：

```c
uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}
```

参数对应：

```text
argfd(0)   从 a0 读取 fd = 1，并找到 struct file
argaddr(1) 从 a1 读取 buf 用户地址
argint(2)  从 a2 读取 n
```

最后 `filewrite()` 会根据文件类型写到 console、pipe 或 inode。

返回值一路写回：

```text
sys_write() 返回写入字节数
-> syscall() 写入 p->trapframe->a0
-> userret 恢复 a0
-> 用户态 write() 返回
```

## 10. 返回用户态：`usertrapret()`

系统调用处理完后，`usertrap()` 最后调用：

```c
usertrapret();
```

它负责准备返回用户态。

### 设置下一次用户 trap 入口

```c
w_stvec(TRAMPOLINE + (uservec - trampoline));
```

返回用户态后，如果再次发生系统调用或中断，要从 `trampoline.S:uservec` 进入。

### 填写 trapframe 中的内核入口信息

```c
p->trapframe->kernel_satp = r_satp();
p->trapframe->kernel_sp = p->kstack + PGSIZE;
p->trapframe->kernel_trap = (uint64)usertrap;
p->trapframe->kernel_hartid = r_tp();
```

这些字段供下一次 `uservec` 使用。

### 设置 `sstatus` 和 `sepc`

```c
x &= ~SSTATUS_SPP;
x |= SSTATUS_SPIE;
w_sstatus(x);
w_sepc(p->trapframe->epc);
```

含义：

```text
SSTATUS_SPP = 0: sret 后返回 user mode
SSTATUS_SPIE = 1: 回到用户态后打开中断
sepc = 用户程序下一条要执行的指令
```

对于系统调用，`epc` 已经在 `usertrap()` 里加过 4，所以会返回到 `ecall` 后面的指令。

### 调用 `userret`

```c
uint64 satp = MAKE_SATP(p->pagetable);
uint64 fn = TRAMPOLINE + (userret - trampoline);
((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
```

传给 `userret` 两个参数：

```text
a0 = TRAPFRAME
a1 = 用户页表 satp
```

## 11. `trampoline.S:userret`

`userret` 负责最后真正切回用户态。

### 切换到用户页表

```asm
csrw satp, a1
sfence.vma zero, zero
```

从内核页表切回当前进程的用户页表。

### 恢复用户寄存器

```asm
ld ra, 40(a0)
ld sp, 48(a0)
...
ld a1, 120(a0)
ld a2, 128(a0)
...
```

从 TRAPFRAME 恢复用户寄存器。

`a0` 特殊处理：

```asm
ld t0, 112(a0)
csrw sscratch, t0
...
csrrw a0, sscratch, a0
```

最终用户态的 `a0` 会变成 `trapframe->a0`。这就是系统调用返回值。

### `sret`

```asm
sret
```

`sret` 根据 `sstatus` 和 `sepc` 返回用户态：

```text
特权级: supervisor -> user
pc: sepc
```

系统调用流程结束。

## 12. 和 pgtbl 实验的关系

系统调用路径和 pgtbl 实验关系很密切。

### `TRAMPOLINE` 必须稳定映射

系统调用进出内核都要执行 trampoline 代码。它必须同时映射在：

```text
用户页表 p->pagetable
内核页表 kernel_pagetable 或 p->kpagetable
```

并且虚拟地址必须都是 `TRAMPOLINE`。

### `TRAPFRAME` 保存参数和返回值

系统调用号和参数都保存在：

```text
p->trapframe->a0 ~ a7
```

返回值也写回：

```text
p->trapframe->a0
```

所以 `TRAPFRAME` 映射和内容正确性直接决定系统调用能否工作。

### `copyin/copyinstr/copyout`

系统调用经常需要访问用户指针，例如：

```text
write(fd, user_buf, n)
exec(path, argv)
read(fd, user_buf, n)
```

原版 xv6 用 `walkaddr()` 手工翻译用户地址。

pgtbl 实验的 `simplify copyin/copyinstr` 希望当前进程的内核页表也映射用户页，这样 `copyin_new()` 可以直接访问用户虚拟地址。

这要求：

```text
scheduler 切到 p->kpagetable
fork/exec/sbrk 正确同步用户页映射到 p->kpagetable
copyin/copyinstr 接入 copyin_new/copyinstr_new
```

## 13. 一张完整时序图

```text
用户代码调用 write()
  |
  v
user/usys.S:write
  li a7, SYS_write
  ecall
  |
  v
硬件 trap
  sepc = ecall 地址
  scause = 8
  pc = stvec = TRAMPOLINE:uservec
  |
  v
trampoline.S:uservec
  保存用户寄存器到 TRAPFRAME
  切到 kernel stack
  切到 kernel page table
  跳到 usertrap()
  |
  v
trap.c:usertrap()
  判断 scause == 8
  epc += 4
  syscall()
  |
  v
syscall.c:syscall()
  num = trapframe->a7
  调用 syscalls[num]()
  返回值写入 trapframe->a0
  |
  v
sysfile.c:sys_write()
  读取参数
  filewrite()
  |
  v
trap.c:usertrapret()
  设置 stvec
  设置 trapframe kernel_* 字段
  设置 sstatus/sepc
  调用 TRAMPOLINE:userret
  |
  v
trampoline.S:userret
  切回用户页表
  恢复用户寄存器
  sret
  |
  v
用户代码继续执行
  write() 返回值在 a0
```
