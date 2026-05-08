# xv6 Trap 流程梳理

本文档梳理 xv6-riscv 中 trap 的完整过程，重点解释用户态进入内核、内核返回用户态、`trapframe`、`trampoline.S`、`sscratch`、`satp`、`sepc/scause/stval` 的关系。

## 一、什么是 trap

trap 是从当前执行流跳到内核处理逻辑的一类事件。

常见 trap 来源：

```text
system call:
  用户程序执行 ecall，请求内核服务。

exception:
  例如 page fault、非法指令、除零等异常。

interrupt:
  例如 timer interrupt、设备中断。
```

在 xv6 中，从用户态进入内核后通常会走：

```text
uservec
-> usertrap()
-> usertrapret()
-> userret
-> sret
```

## 二、相关文件

主要文件：

```text
kernel/trampoline.S
kernel/trap.c
kernel/proc.h
kernel/riscv.h
kernel/kernelvec.S
```

它们的职责：

```text
trampoline.S:
  用户态和内核态切换时最底层的汇编代码。

trap.c:
  trap 的 C 语言处理逻辑。

proc.h:
  定义 trapframe 和 proc。

riscv.h:
  封装 CSR 读写，例如 r_scause(), w_sepc(), w_satp()。

kernelvec.S:
  内核态发生 trap 时的入口。
```

## 三、几个关键寄存器和结构

### `stvec`

`stvec` 保存 trap 入口地址。

用户态运行前，xv6 设置：

```c
w_stvec(TRAMPOLINE + (uservec - trampoline));
```

含义：

```text
如果用户态发生 trap，CPU 跳到 trampoline.S 的 uservec。
```

内核态运行时，xv6 设置：

```c
w_stvec((uint64)kernelvec);
```

含义：

```text
如果内核态发生 trap，CPU 跳到 kernelvec。
```

### `sepc`

`sepc` 保存 trap 发生时的 PC。

例如用户程序执行：

```asm
ecall
```

进入内核后：

```text
sepc = ecall 指令地址
```

系统调用处理后通常要：

```c
p->trapframe->epc += 4;
```

因为 `ecall` 是 4 字节，返回用户态时要跳过这条指令。

### `scause`

`scause` 保存 trap 原因。

常见值：

```text
8:
  user ecall，系统调用。

12:
  instruction page fault。

13:
  load page fault。

15:
  store page fault。
```

### `stval`

`stval` 保存和异常相关的值。

例如 page fault 时：

```text
stval = 触发 fault 的虚拟地址
```

调试时常看：

```text
scause
sepc
stval
```

### `satp`

`satp` 保存当前页表根地址。

切页表时：

```c
w_satp(MAKE_SATP(pagetable));
sfence_vma();
```

`sfence_vma()` 用于刷新 TLB。

### `sscratch`

`sscratch` 是 supervisor scratch CSR。

在 xv6 的用户态 trap 路径中：

```text
用户态运行期间:
  sscratch = TRAPFRAME

进入 uservec 后:
  sscratch = 用户原来的 a0
  a0 = TRAPFRAME
```

它的作用是：

```text
让 uservec 能拿到 trapframe 地址，
同时不丢失用户原来的 a0。
```

### `trapframe`

`trapframe` 定义在：

```text
kernel/proc.h
```

它保存一次用户态 trap 所需的所有用户寄存器，以及内核恢复所需的信息。

重要字段：

```text
kernel_satp:
  trap 进入内核时要切换到的内核页表。

kernel_sp:
  当前进程的 kernel stack 栈顶。

kernel_trap:
  usertrap() 的地址。

kernel_hartid:
  当前 CPU/hart id，用于恢复 tp。

epc:
  用户态返回地址。

ra/sp/gp/tp/a0...:
  用户态通用寄存器。
```

## 四、为什么需要 trampoline

用户态发生 trap 时，CPU 做的事情很少：

```text
1. 切换到 supervisor mode。
2. 把当前 PC 保存到 sepc。
3. 设置 scause/stval。
4. 跳到 stvec 指向的地址。
```

CPU 不会自动：

```text
保存所有用户寄存器
切换页表
切换内核栈
调用 C 函数
```

所以 xv6 需要 `trampoline.S` 做这些底层工作。

`trampoline` 必须同时映射在：

```text
用户页表
内核页表
```

并且虚拟地址相同：

```text
TRAMPOLINE
```

原因：

trap 入口刚开始还在使用用户页表，后面要切换到内核页表。代码必须在切页表前后都能继续执行。

## 五、用户态进入内核完整流程

假设用户程序执行系统调用：

```asm
ecall
```

### 1. CPU 硬件进入 supervisor mode

CPU 自动做：

```text
sepc = 当前用户 PC
scause = trap 原因
sstatus.SPP = User
pc = stvec
进入 supervisor mode
```

此时仍然使用：

```text
用户页表
```

还没有切换到内核页表。

### 2. 跳到 `uservec`

入口在：

```text
kernel/trampoline.S
```

关键代码：

```asm
csrrw a0, sscratch, a0
```

进入前：

```text
a0 = 用户程序自己的 a0
sscratch = TRAPFRAME
```

交换后：

```text
a0 = TRAPFRAME
sscratch = 用户 a0
```

现在 `uservec` 可以用 `a0` 作为 trapframe 地址。

### 3. 保存用户寄存器

`uservec` 把用户寄存器保存到 trapframe：

```asm
sd ra, 40(a0)
sd sp, 48(a0)
...
sd a7, 168(a0)
...
```

用户原来的 `a0` 之前被放进 `sscratch`，现在再保存：

```asm
csrr t0, sscratch
sd t0, 112(a0)
```

此时用户寄存器已经完整保存在：

```text
p->trapframe
```

### 4. 恢复内核运行所需状态

`uservec` 从 trapframe 取出：

```asm
ld sp, 8(a0)     # kernel_sp
ld tp, 32(a0)    # kernel_hartid
ld t0, 16(a0)    # kernel_trap
ld t1, 0(a0)     # kernel_satp
```

含义：

```text
sp = 当前进程 kernel stack
tp = 当前 hartid
t0 = usertrap() 地址
t1 = 内核页表 satp
```

### 5. 切换到内核页表

```asm
csrw satp, t1
sfence.vma zero, zero
```

现在 CPU 使用内核页表。

如果做了 pgtbl 实验，这里的 `kernel_satp` 应该是：

```text
MAKE_SATP(p->kpagetable)
```

而不是全局 `kernel_pagetable`。

### 6. 跳到 `usertrap()`

```asm
jr t0
```

进入：

```text
kernel/trap.c:usertrap()
```

## 六、`usertrap()` 做什么

位置：

```text
kernel/trap.c
```

主要步骤：

### 1. 确认来自用户态

```c
if((r_sstatus() & SSTATUS_SPP) != 0)
  panic("usertrap: not from user mode");
```

`SPP == 0` 表示 trap 前是 user mode。

### 2. 设置内核态 trap 入口

```c
w_stvec((uint64)kernelvec);
```

因为现在已经在内核态。如果内核里再发生 trap，应该走 `kernelvec`。

### 3. 保存用户 PC

```c
p->trapframe->epc = r_sepc();
```

后面返回用户态时会用。

### 4. 判断 trap 原因

系统调用：

```c
if(r_scause() == 8){
  p->trapframe->epc += 4;
  intr_on();
  syscall();
}
```

设备中断：

```c
else if((which_dev = devintr()) != 0){
  ...
}
```

异常：

```c
else {
  printf("usertrap(): unexpected scause ...");
  p->killed = 1;
}
```

### 5. timer interrupt 触发调度

如果是 timer interrupt：

```c
if(which_dev == 2)
  yield();
```

这会让当前进程让出 CPU。

### 6. 返回用户态

```c
usertrapret();
```

## 七、内核返回用户态完整流程

### 1. `usertrapret()` 准备返回

位置：

```text
kernel/trap.c
```

先关中断：

```c
intr_off();
```

因为接下来要把 trap 入口改回 `uservec`。

### 2. 设置下一次用户 trap 的入口

```c
w_stvec(TRAMPOLINE + (uservec - trampoline));
```

含义：

```text
下一次用户态 trap，跳到 uservec。
```

### 3. 填充 trapframe 中的内核信息

```c
p->trapframe->kernel_satp = MAKE_SATP(kernel_pagetable);
p->trapframe->kernel_sp = p->kstack + PGSIZE;
p->trapframe->kernel_trap = (uint64)usertrap;
p->trapframe->kernel_hartid = r_tp();
```

如果做了 pgtbl 实验，第一行应是：

```c
p->trapframe->kernel_satp = MAKE_SATP(p->kpagetable);
```

原因：

下一次用户态 trap 进内核时，`uservec` 要切到当前进程自己的内核页表。

### 4. 设置 `sstatus`

```c
unsigned long x = r_sstatus();
x &= ~SSTATUS_SPP;
x |= SSTATUS_SPIE;
w_sstatus(x);
```

含义：

```text
SPP = 0:
  sret 返回 user mode。

SPIE = 1:
  回用户态后打开中断。
```

### 5. 设置返回地址 `sepc`

```c
w_sepc(p->trapframe->epc);
```

`sret` 会跳到 `sepc`。

### 6. 调用 `userret`

```c
uint64 satp = MAKE_SATP(p->pagetable);
uint64 fn = TRAMPOLINE + (userret - trampoline);
((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
```

传给 `userret`：

```text
a0 = TRAPFRAME
a1 = 用户页表 satp
```

## 八、`userret` 做什么

位置：

```text
kernel/trampoline.S
```

### 1. 切到用户页表

```asm
csrw satp, a1
sfence.vma zero, zero
```

现在 CPU 使用用户页表。

### 2. 准备 `sscratch`

```asm
ld t0, 112(a0)
csrw sscratch, t0
```

这里先把用户原来的 `a0` 放到 `sscratch`。

### 3. 恢复除 `a0` 外的用户寄存器

```asm
ld ra, 40(a0)
ld sp, 48(a0)
...
ld a1, 120(a0)
...
```

暂时不恢复 `a0`，因为当前 `a0` 还要作为 `TRAPFRAME` 地址使用。

### 4. 最后恢复用户 `a0`，并把 TRAPFRAME 存进 `sscratch`

```asm
csrrw a0, sscratch, a0
```

执行前：

```text
a0 = TRAPFRAME
sscratch = 用户 a0
```

执行后：

```text
a0 = 用户 a0
sscratch = TRAPFRAME
```

这保证下一次用户态 trap 时，`uservec` 又能从 `sscratch` 拿到 `TRAPFRAME`。

### 5. `sret`

```asm
sret
```

CPU 根据：

```text
sepc
sstatus.SPP
sstatus.SPIE
```

返回用户态。

## 九、内核态 trap

如果内核态发生 trap，不走 `uservec`，而是走：

```text
kernelvec
-> kerneltrap()
```

因为内核态运行时：

```c
w_stvec((uint64)kernelvec);
```

`kerneltrap()` 会处理内核态 timer/device interrupt。

如果内核态发生 page fault，通常说明内核 bug，会看到：

```text
panic: kerneltrap
```

例如 pgtbl 实验中 `copyin_new()` 直接访问未映射用户地址，就会在内核态触发：

```text
scause = 13
stval = 用户虚拟地址
panic: kerneltrap
```

## 十、系统调用 trap 示例

用户代码：

```c
write(1, "hi", 2);
```

大致过程：

```text
1. user/usys.S 中执行 ecall。
2. CPU 进入 supervisor mode，跳到 uservec。
3. uservec 保存用户寄存器。
4. uservec 切换到内核页表和 kernel stack。
5. uservec 跳到 usertrap()。
6. usertrap() 发现 scause == 8。
7. usertrap() 令 epc += 4，跳过 ecall。
8. syscall() 根据 a7 中的 syscall number 调用 sys_write()。
9. sys_write() 完成后返回值写到 trapframe->a0。
10. usertrapret() 准备返回用户态。
11. userret 切回用户页表，恢复寄存器。
12. sret 回到 ecall 后一条用户指令。
```

## 十一、timer interrupt trap 示例

用户程序运行时发生 timer interrupt：

```text
1. CPU 跳到 uservec。
2. 保存用户寄存器。
3. 切到内核页表。
4. 进入 usertrap()。
5. devintr() 判断是 timer interrupt。
6. usertrap() 调用 yield()。
7. 调度器可能切换到其他进程。
8. 当前进程未来继续运行时，从 trapframe 恢复用户寄存器。
```

traps 实验中的 `sigalarm` 就是在这个路径上修改：

```text
当 timer tick 数达到 interval，
把 p->trapframe->epc 改成 handler 地址。
```

这样返回用户态时，不是回到原 PC，而是跳到用户注册的 handler。

## 十二、和 pgtbl 实验的联系

pgtbl 实验中每个进程有自己的内核页表：

```text
p->kpagetable
```

因此返回用户态前必须设置：

```c
p->trapframe->kernel_satp = MAKE_SATP(p->kpagetable);
```

否则下一次从用户态 trap 进内核时，`uservec` 会切到错误的内核页表。

如果 `copyin_new()` 直接访问用户虚拟地址，而当前 `satp` 不是 `p->kpagetable`，就会出现：

```text
scause = 13
sepc = memmove
stval = 用户地址
panic: kerneltrap
```

## 十三、调试 trap 的方法

看到 trap 错误时重点看：

```text
scause:
  trap 原因。

sepc:
  trap 发生时的 PC。

stval:
  fault 地址或相关值。
```

常见判断：

```text
scause = 8:
  用户系统调用。

scause = 12:
  instruction page fault。

scause = 13:
  load page fault。

scause = 15:
  store page fault。
```

如果是内核地址：

```text
sepc = 0x8000....
```

说明 fault 发生在内核代码。

可以用：

```bash
riscv64-unknown-elf-addr2line -e kernel/kernel 0x8000....
```

定位到源码行。

也可以用：

```bash
riscv64-unknown-elf-objdump -d kernel/kernel
```

查看具体汇编指令。

## 十四、总结

用户态 trap 进入内核：

```text
user mode
-> hardware trap
-> uservec
-> 保存用户寄存器到 trapframe
-> 切换内核页表
-> 切换 kernel stack
-> usertrap()
```

内核返回用户态：

```text
usertrapret()
-> 设置 stvec/sepc/sstatus/trapframe
-> userret
-> 切换用户页表
-> 恢复用户寄存器
-> sret
-> user mode
```

最关键的结构是：

```text
trapframe:
  保存用户寄存器和内核恢复信息。

trampoline:
  在用户页表和内核页表中同 VA 映射，负责切换页表时继续执行。

sscratch:
  用户态时保存 TRAPFRAME，trap 入口时帮助交换 a0。

satp:
  决定当前地址空间。
```
