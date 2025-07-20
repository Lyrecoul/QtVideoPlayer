# NewPlayer

## 简介

这是一个适用于带 ffmpeg 3.4.8 版本的 rk3326 网易有道词典笔 2 的 Qt 视频播放器。

## 编译

1. 克隆本仓库（需要安装 Git LFS 以处理大文件）

```shell
git clone https://github.com/Lyrecoul/QtVideoPlayer.git
git lfs install
git lfs pull
```

2. 克隆 Qt 仓库、gcc 工具链以及词典笔 /usr/libs 文件夹

```shell
git clone https://github.com/Lyrecoul/qt-5.15.2-for-aarch64-dictpen-linux.git
git clone https://github.com/Lyrecoul/aarch64-dictpen-linux-gnu-gcc-toolchain.git
git clone https://github.com/Lyrecoul/dictpen-libs.git
```

3. 将 gcc 工具链添加到 PATH 环境变量

选择适合当前用户使用的 shell 对应命令并执行。

```shell
echo 'export PATH="$HOME/[工具链/bin]:$PATH"' >> ~/.bash_profile

echo 'export PATH="$HOME/[工具链/bin]:$PATH"' >> ~/.zshenv

fish_add_path -g -p ~/[工具链/bin]

echo 'setenv PATH "$HOME/[工具链/bin]:$PATH"' >> ~/.cshrc

echo 'setenv PATH "$HOME/[工具链/bin]:$PATH"' >> ~/.tcshrc

echo 'export PATH="$HOME/[工具链/bin]:$PATH"' >> ~/.profile

echo 'export PATH="$HOME/[工具链/bin]:$PATH"' >> ~/.profile
```

重启终端以使环境变量生效。

4. 编译

```shell
./qt-5.15.2-for-aarch64-dictpen-linux/bin/qmake NewPlayer.pro
make -j$(nproc)
```

5. 剥离调试符号

```shell
aarch64-dictpen-linux-gnu-strip NewPlayer
```

## 更新日志

详见 [更新日志](https://github.com/Lyrecoul/QtVideoPlayer/blob/main/ChangeLog.txt)

## 鸣谢以下开源项目

- [Qt](https://www.qt.io/)
- [FFmpeg](https://ffmpeg.org/)
