# Git 操作命令详解

本文档面向当前 xv6 实验仓库的使用场景，重点解释如何安全保存实验代码、切换实验分支、撤销误操作，以及如何理解 `git add .` 之后的状态。

## 一、Git 的三个区域

理解 Git 命令前，先区分三个区域：

```text
工作区 working tree:
  你当前目录里的真实文件。

暂存区 staging area / index:
  git add 后进入的区域，表示“准备提交的内容”。

本地仓库 repository:
  git commit 后真正保存到 Git 历史里的内容。
```

关系是：

```text
修改文件
  -> 工作区变化

git add
  -> 放入暂存区

git commit
  -> 保存到本地仓库历史
```

所以：

```text
git add . 不等于保存完成。
git commit 才是真正形成一个可切换、可恢复的历史点。
```

## 二、查看状态

### `git status`

查看当前仓库状态：

```bash
git status
```

简洁版本：

```bash
git status --short
```

常见标记：

```text
 M file:
  文件在工作区被修改，但还没 add。

M  file:
  文件已经 add 到暂存区。

A  file:
  新文件已经 add 到暂存区。

?? file:
  新文件还没有被 Git 跟踪，也没有 add。
```

如果看到很多文件都是：

```text
M  ...
A  ...
```

说明它们已经被 `git add` 放进暂存区。

## 三、查看改动

### 查看未暂存改动

```bash
git diff
```

查看的是：

```text
工作区中还没 git add 的改动。
```

### 查看已暂存改动

```bash
git diff --cached
```

查看的是：

```text
已经 git add，准备 commit 的改动。
```

### 查看统计

```bash
git diff --stat
```

查看未暂存改动统计。

```bash
git diff --cached --stat
```

查看暂存区改动统计。

在误执行 `git add .` 后，推荐先跑：

```bash
git diff --cached --stat
```

确认到底加进去了哪些文件。

## 四、添加文件

### `git add file`

添加单个文件：

```bash
git add kernel/vm.c
```

适合精确控制提交内容。

### `git add dir`

添加目录：

```bash
git add docs
```

### `git add .`

添加当前目录下所有变化：

```bash
git add .
```

风险：

```text
会把不该提交的文件也加进去，例如 .idea、临时文件、编译产物、个人笔记。
```

实验仓库里更推荐：

```bash
git add kernel/vm.c kernel/proc.c kernel/exec.c kernel/trap.c kernel/defs.h kernel/proc.h
git add docs
git add answers-pgtbl.txt time.txt
```

这样更容易控制提交内容。

## 五、撤销 `git add`

如果你执行了：

```bash
git add .
```

但不想让这些内容都进入暂存区，可以撤销暂存。

### 撤销所有暂存

```bash
git restore --staged .
```

效果：

```text
文件内容不会丢。
只是从暂存区移回工作区。
```

也就是：

```text
M  file
变回
 M file
```

### 撤销单个文件暂存

```bash
git restore --staged kernel/vm.c
```

旧版本 Git 可以用：

```bash
git reset HEAD kernel/vm.c
```

注意：

```text
git restore --staged 不会删除你的代码改动。
它只是不把这些改动放进下一次 commit。
```

## 六、提交改动

### 普通提交

```bash
git commit -m "finish pgtbl lab"
```

含义：

```text
把暂存区内容保存成本地 Git 历史中的一个提交。
```

提交后可以安全切分支，因为当前工作可以通过 commit 找回。

### 提交前建议

先看状态：

```bash
git status --short
```

再看暂存内容：

```bash
git diff --cached --stat
```

如果发现 `.idea/`、`.codex`、临时文件也被 add 了，先撤销：

```bash
git restore --staged .idea .codex
```

然后再 commit。

## 七、切换分支

### 查看分支

```bash
git branch
```

当前分支前面会有：

```text
*
```

### 切换分支

```bash
git switch traps
```

旧命令：

```bash
git checkout traps
```

推荐新命令：

```bash
git switch
```

因为它语义更清楚，只负责切分支。

### 切分支前必须确认

```bash
git status
```

如果有未提交改动，Git 可能：

```text
1. 拒绝切换，提示会覆盖文件。
2. 允许切换，但把改动带到新分支。
```

所以切分支前最好：

```text
commit
或 stash
```

## 八、临时保存改动：stash

如果你还不想 commit，但想切分支，可以用 `stash`。

### 保存当前改动

```bash
git stash push -m "pgtbl work before traps"
```

效果：

```text
把当前工作区和暂存区改动临时收起来。
工作区恢复干净。
```

然后可以切分支：

```bash
git switch traps
```

### 查看 stash

```bash
git stash list
```

### 恢复最近一次 stash

```bash
git stash pop
```

含义：

```text
恢复 stash，并从 stash 列表中删除它。
```

### 只恢复但不删除 stash

```bash
git stash apply
```

### 删除某个 stash

```bash
git stash drop stash@{0}
```

注意：

```text
stash 适合临时切换任务。
长期保存实验成果，commit 更可靠。
```

## 九、当前这种情况如何处理

如果你已经执行了：

```bash
git add .
```

现在想切到 `traps` 分支，不建议直接切。

推荐流程一：提交当前 pgtbl 工作

```bash
git diff --cached --stat
git restore --staged .idea .codex
git add docs
git commit -m "pgtbl lab progress"
git switch traps
```

适合：

```text
当前 pgtbl 工作希望保存到历史里。
```

推荐流程二：暂存起来再切

```bash
git stash push -m "pgtbl work before traps"
git switch traps
```

适合：

```text
当前代码还没整理好，不想 commit。
```

如果后面要回来：

```bash
git switch pgtbl
git stash pop
```

## 十、撤销文件改动

危险操作，要谨慎。

### 丢弃某个文件的工作区改动

```bash
git restore kernel/vm.c
```

含义：

```text
把 kernel/vm.c 恢复到当前分支 HEAD 的状态。
未提交改动会丢失。
```

### 丢弃所有工作区改动

```bash
git restore .
```

危险：

```text
当前所有未提交改动都会丢失。
```

不要随便执行。

### 从暂存区移除，但保留改动

```bash
git restore --staged .
```

安全得多：

```text
不会丢代码，只是不准备提交。
```

## 十一、不要随便用的命令

### `git reset --hard`

```bash
git reset --hard
```

危险：

```text
会丢弃所有未提交改动。
```

除非非常确定，否则不要用。

### `git clean -fd`

```bash
git clean -fd
```

危险：

```text
会删除所有未跟踪文件。
```

例如：

```text
docs/
note
临时文件
```

可能直接被删。

## 十二、推荐日常工作流

做实验时推荐：

```bash
git status --short
```

修改一部分后：

```bash
git diff
```

确认没问题后精确 add：

```bash
git add kernel/vm.c kernel/proc.c
```

查看暂存内容：

```bash
git diff --cached --stat
```

提交：

```bash
git commit -m "implement per-process kernel page table"
```

继续下一部分。

## 十三、pgtbl 切 traps 的建议

如果当前 pgtbl 还没有完全通过测试，但你想先看 traps：

```bash
git stash push -m "pgtbl unfinished before traps"
git switch traps
```

如果当前 pgtbl 想作为阶段成果保存：

```bash
git restore --staged .idea .codex
git diff --cached --stat
git commit -m "pgtbl lab progress"
git switch traps
```

一句话总结：

```text
git add . 只是准备提交，不是保存。
切分支前，要么 commit，要么 stash。
不要在大量 staged 改动的情况下直接 switch。
```
