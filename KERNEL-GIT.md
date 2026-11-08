# 内核 Git 与子模块

本仓库根目录是 `pushup/w202ds-linux/6.18.35/`，主仓库和内核子模块均在 `6.18.35` 分支。
主仓库保留原有提交历史；`kernel/` 以官方 Linux stable `v6.18.35` 为父提交，记录现有 UMS9620/W202DS 适配。
官方基线采用深度为 1 的浅克隆，没有下载整个 Linux 历史。

上游：`https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git`。
W202DS 内核仓库：`https://github.com/Yeamika/w202ds-linux-kernel.git`，分支 `6.18.35`。
基线提交：`acb7cf4c1184e27622be0faf89244d5001ed1e87`。
当前适配提交：`53bb26e2e0d531ba4113eaa898cfdfb3765537c1`。
本轮新提交的作者和提交者均使用 `Yeamika <255833500+Yeamika@users.noreply.github.com>`。

## 检查与继续开发

从本仓库根目录运行：

```sh
git branch --show-current
git submodule status kernel
git -C kernel status --short
git -C kernel diff --stat v6.18.35..HEAD
git -C kernel log --oneline v6.18.35..HEAD
```

先在 `kernel/` 提交内核改动，再在主仓库 `git add kernel` 并提交新指针。
`Devices/` 中的模块实现、配置和设备树由主仓库维护，不属于内核子模块提交。
内核仍有 4 个相对链接接入这些实现，构建必须保留同级 `Devices/` 布局。
构建命令见 [BUILD.md](BUILD.md)。`CONFIG_LOCALVERSION_AUTO=y` 启用内核 Git 版本信息；
示例中的 `LOCALVERSION=` 仅表示不追加手工后缀，不会关闭自动 Git 标识。

## 从 GitHub 检出

`.gitmodules` 指向 Yeamika 的 Linux stable fork，主仓库固定 W202DS 适配提交：

```sh
git clone --branch 6.18.35 --recurse-submodules --shallow-submodules \
  https://github.com/Yeamika/w202ds-linux.git w202ds-linux/6.18.35
```

已有主仓库检出可运行 `git submodule update --init --depth=1 kernel`。
开发时子模块的 `origin` 指向 W202DS fork，`upstream` 指向官方 Linux stable；
先推送内核提交，再提交并推送主仓库的子模块指针。

## 增量 bundle 备用恢复

`kernel-local.bundle` 保留首次内核提交相对官方基线的增量对象和分支，可恢复同一提交 ID。

在一个新的主仓库检出中，先取得官方基线，再导入 bundle：

```sh
git submodule init kernel
git clone --depth=1 --branch v6.18.35 --no-checkout \
  https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git kernel
git -C kernel fetch ../kernel-local.bundle refs/heads/6.18.35:refs/heads/6.18.35
git -C kernel switch 6.18.35
git submodule absorbgitdirs kernel
git submodule status kernel
```

此流程用于尚未初始化的空 `kernel/` 目录；已有工作区直接使用现有子模块。
这份 bundle 仅覆盖首次内核提交，后续内核提交由 W202DS 远端提供；
如果需要沿用 bundle 恢复，也应同步更新 bundle。上层仓库的其他源码不由该 bundle 保存。
