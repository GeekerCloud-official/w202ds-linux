# 内核 Git 与子模块

本仓库根目录是 `pushup/w202ds-linux/6.18.35/`，主仓库和内核子模块均在 `6.18.35` 分支。
主仓库保留原有提交历史；`kernel/` 以官方 Linux stable `v6.18.35` 为父提交，记录现有 UMS9620/W202DS 适配。
官方基线采用深度为 1 的浅克隆，没有下载整个 Linux 历史。

上游：`https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git`。
基线提交：`acb7cf4c1184e27622be0faf89244d5001ed1e87`。
当前适配提交：`815a26419264a57cb9a7a8f4f4bf2869fd70048a`，验证记录见 [provenance](provenance/kernel-git-20261105/README.md)。
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
内核仍有 5 个相对链接接入这些实现，构建必须保留同级 `Devices/` 布局。
构建命令见 [BUILD.md](BUILD.md)，显式设置 `LOCALVERSION=`，防止新 Git 提交使内核版本多出 `+`。

## 未推送提交的恢复

本次只创建本地提交，没有推送或创建远端 fork。`.gitmodules` 仍指向官方上游，
官方服务器不包含本地 W202DS 提交，因此不能只靠 `git submodule update --init` 恢复它。
`kernel-local.bundle` 保存相对官方基线的增量对象和分支，可恢复同一提交 ID。

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
这份 bundle 仅覆盖本次提交，后续内核提交需要更新 bundle，或先推送到自己的内核远端，
再把 `.gitmodules` 的 URL 改为该远端。上层仓库的其他未提交源码不由该 bundle 保存。
