# fs 分支当前修改总结

当前分支：`fs`

当前实验：xv6-labs-2020 `fs` lab

当前分支主要围绕两个任务修改：

1. 支持大文件，也就是为 inode 增加 double indirect block。
2. 支持 symbolic link，也就是实现 `symlink()` 系统调用和 `O_NOFOLLOW`。

另外还修复了新版 Python 下 `make grade` 的脚本兼容问题。

## 一、bigfile 相关修改

### 1. 修改文件最大块数定义

修改文件：`kernel/fs.h`

原始 xv6 的 inode 地址布局是：

```text
addrs[0..11]  direct blocks
addrs[12]     single indirect block
```

原始最大文件块数：

```text
12 + 256 = 268 blocks
```

当前修改为：

```c
#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))
#define NINDIRECT2 (NINDIRECT * NINDIRECT)
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT2)
```

新的地址布局是：

```text
addrs[0..10]  direct blocks
addrs[11]     single indirect block
addrs[12]     double indirect block
```

这样最大文件块数变成：

```text
11 + 256 + 256*256 = 65803 blocks
```

这正好对应 `grade-lab-fs` 里 `bigfile` 期望输出：

```text
wrote 65803 blocks
```

### 2. 同步磁盘 inode 和内存 inode

修改文件：

```text
kernel/fs.h
kernel/file.h
```

磁盘 inode：

```c
struct dinode {
  ...
  uint addrs[NDIRECT+2];
};
```

内存 inode：

```c
struct inode {
  ...
  uint addrs[NDIRECT+2];
};
```

这两个必须一致。`dinode` 是写在磁盘上的 inode 格式，`inode` 是内核运行时缓存的 inode 副本。`ilock()` 会从 `dinode` 复制到 `inode`，`iupdate()` 会从 `inode` 写回 `dinode`。

如果一个是 `NDIRECT+1`，另一个是 `NDIRECT+2`，就会导致地址数组布局不一致，严重时会越界或写坏磁盘 inode。

### 3. 扩展 bmap 支持 double indirect

修改文件：`kernel/fs.c`

`bmap(ip, bn)` 的作用是：

```text
把文件内部第 bn 个 block 映射成磁盘上的真实 block number。
如果还没有对应磁盘块，就分配。
```

当前 `bmap()` 支持三类块：

```text
bn < NDIRECT
  使用 direct block。

bn < NDIRECT + NINDIRECT
  使用 single indirect block。

bn < NDIRECT + NINDIRECT + NINDIRECT2
  使用 double indirect block。
```

double indirect 的寻址结构是：

```text
ip->addrs[NDIRECT+1]
  -> double indirect block
       idx1 -> second-level indirect block
                 idx2 -> data block
```

其中：

```c
uint idx1 = bn / NINDIRECT;
uint idx2 = bn % NINDIRECT;
```

当前实现已经避免了两个常见问题：

```text
1. 修改一级 double indirect block 后调用 log_write(bp)。
2. 读取第二级 indirect block 前释放第一个 bp，避免 buffer 泄漏。
```

### 4. 扩展 itrunc 释放 double indirect

修改文件：`kernel/fs.c`

`itrunc(ip)` 的作用是释放文件占用的所有数据块，并把文件大小清零。

当前释放顺序是：

```text
1. 释放 direct data blocks。
2. 释放 single indirect 指向的 data blocks。
3. 释放 single indirect block 本身。
4. 释放 double indirect 管理的所有 data blocks。
5. 释放 double indirect 下的 second-level indirect blocks。
6. 释放 double indirect block 本身。
7. ip->size = 0。
8. iupdate(ip) 写回 inode。
```

如果不修改 `itrunc()`，大文件删除时会泄漏大量磁盘 block。

### 5. 扩展 mkfs 的 iappend

修改文件：`mkfs/mkfs.c`

`iappend(inum, data, n)` 运行在宿主机上的 `mkfs` 工具中，用于创建 `fs.img` 时向某个 inode 追加数据。

它和内核里的 `bmap()` 类似，也必须理解新的 inode 地址布局：

```text
direct
single indirect
double indirect
```

当前修改增加了：

```c
uint indirect[NINDIRECT], indirect2[NINDIRECT];
```

double indirect 分支中：

```text
1. 如果 din.addrs[NDIRECT+1] 不存在，分配 double indirect block。
2. 读取 double indirect block 到 indirect[]。
3. 根据一级索引找到或分配 second-level indirect block。
4. 读取 second-level indirect block 到 indirect2[]。
5. 根据二级索引找到或分配 data block。
6. 设置 x 为最终 data block number。
```

这里必须用两个数组或保存中间 blockno。否则读第二级 indirect block 时会覆盖第一级 indirect block 内容，导致后续地址计算错误。

## 二、symlink 相关修改

### 1. 新增文件类型

修改文件：`kernel/stat.h`

新增：

```c
#define T_SYMLINK 4
```

含义：

```text
这个 inode 是 symbolic link。
```

`symlinktest` 会通过：

```c
open(path, O_RDONLY | O_NOFOLLOW)
fstat(fd, &st)
```

检查 `st.type == T_SYMLINK`。

### 2. 新增 open flag

修改文件：`kernel/fcntl.h`

新增：

```c
#define O_NOFOLLOW 0x800
```

含义：

```text
open 遇到符号链接时，不跟随目标，而是打开 symlink inode 本身。
```

默认情况下：

```text
open("/link")
  如果 /link 是 T_SYMLINK
  读取 link 里面保存的 target path
  再打开 target
```

带 `O_NOFOLLOW` 时：

```text
open("/link", O_NOFOLLOW)
  直接打开 /link 这个符号链接本身
```

### 3. 添加 symlink 系统调用

修改文件：

```text
kernel/syscall.h
kernel/syscall.c
user/user.h
user/usys.pl
kernel/sysfile.c
```

系统调用编号：

```c
#define SYS_symlink 22
```

系统调用表中注册：

```c
extern uint64 sys_symlink(void);
[SYS_symlink] sys_symlink,
```

用户态声明：

```c
int symlink(const char*, const char*);
```

用户态 syscall stub：

```perl
entry("symlink");
```

调用链是：

```text
user symlink(target, path)
  -> user/usys.S stub
  -> a7 = SYS_symlink
  -> ecall
  -> syscall()
  -> sys_symlink()
```

### 4. sys_symlink 的实现

修改文件：`kernel/sysfile.c`

当前实现大致流程：

```text
1. argstr 取 target 和 path。
2. begin_op() 开始文件系统事务。
3. create(path, T_SYMLINK, 0, 0) 创建 symlink inode。
4. writei() 把 target 路径写入 symlink 文件内容。
5. iunlockput(ip) 释放 inode 锁和引用。
6. end_op() 结束事务。
7. 返回 0。
```

符号链接文件的本质是：

```text
type = T_SYMLINK
data block 内容 = target 路径字符串
```

也就是说，target 不是直接存在 inode 的某个字段里，而是作为这个 symlink 文件的内容保存。

### 5. 修改 sys_open 跟随 symlink

修改文件：`kernel/sysfile.c`

当前 `sys_open()` 在 `namei(path)` 和 `ilock(ip)` 后加入了 symlink 跟随逻辑。

核心行为：

```text
如果没有 O_NOFOLLOW：
  while ip->type == T_SYMLINK:
    readi(ip) 读出 target path
    iunlockput(ip)
    ip = namei(target)
    ilock(ip)
```

为了避免符号链接循环：

```text
a -> b
b -> a
```

当前实现设置了最大跟随深度：

```c
if(depth++ >= 10)
  return -1;
```

测试中会覆盖这个场景。

## 三、测试程序集成

修改文件：`Makefile`

fs 实验的用户程序现在包含：

```make
ifeq ($(LAB),fs)
UPROGS += \
	$U/_bigfile\
	$U/_symlinktest
endif
```

这样 `make qemu` 生成 `fs.img` 时会把 `symlinktest` 放进 xv6 文件系统中，进入 xv6 后可以执行：

```sh
symlinktest
```

`grade-lab-fs` 会测试：

```text
bigfile
symlinktest
usertests
```

## 四、grade 脚本兼容性修复

修改文件：`gradelib.py`

原来：

```python
from pipes import quote
```

修改为：

```python
from shlex import quote
```

原因：

```text
新版 Python 移除了 pipes 模块。
如果不改，make grade 会报 ModuleNotFoundError: No module named 'pipes'。
```

## 五、当前实现需要重点验证的风险点

### 1. symlink target 字符串结尾

当前 `sys_symlink()` 写入 target 时使用的是：

```c
writei(ip, 0, (uint64)target, 0, strlen(target))
```

这不会把字符串末尾的 `'\0'` 写进去。

而 `sys_open()` 读取 target 后会把它传给：

```c
namei(target)
```

`namei()` 需要一个以 `'\0'` 结尾的 C 字符串。

因此这里建议改成下面两种方式之一：

```c
writei(ip, 0, (uint64)target, 0, strlen(target) + 1)
```

或者在 `sys_open()` 读出后手动补：

```c
target[n] = 0;
```

第一种更简单。

### 2. sys_open 中错误路径释放要一致

symlink 跟随过程中，如果失败，要确认每条路径都正确释放：

```text
已经 ilock 的 inode 用 iunlockput(ip)
只需要解锁但保留给 file 的 inode 用 iunlock(ip)
```

当前深度超限分支使用的是：

```c
iunlock(ip);
end_op();
return -1;
```

这里失败后不再需要 inode，通常应考虑使用：

```c
iunlockput(ip);
```

否则可能泄漏 inode 引用。

### 3. 需要重新跑测试确认

当前分支应至少验证：

```sh
bigfile
symlinktest
usertests
```

以及：

```sh
make grade
```

如果 `make grade` 因 QEMU 创建本地 gdb socket 报权限错误，需要在非沙箱环境或允许本地 socket 的环境下运行。

## 六、总体修改链路

bigfile 修改链路：

```text
fs.h/file.h 定义新 inode 地址布局
  -> fs.c:bmap 负责按新布局查找或分配 block
  -> fs.c:itrunc 负责按新布局释放 block
  -> mkfs.c:iappend 负责创建 fs.img 时按新布局写文件
```

symlink 修改链路：

```text
user symlink()
  -> usys.S syscall stub
  -> syscall()
  -> sys_symlink()
  -> create(T_SYMLINK)
  -> writei(target path)

open(path)
  -> namei(path)
  -> 如果是 T_SYMLINK 且没有 O_NOFOLLOW
  -> readi 读出 target
  -> namei(target)
  -> 打开最终目标 inode
```

