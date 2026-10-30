# RetroArch for PS4 — audio fix, keyboard & CJK fonts

[English](#english) · [中文](#中文)

A fork of [osirizx/retroarch](https://github.com/osirizx/retroarch) (the PS4
port of [libretro/RetroArch](https://github.com/libretro/RetroArch) 1.8.8),
rebuilt with the [OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
toolchain and three additions, developed and tested on a PS4 Pro (firmware
12.02, GoldHEN). The original port's upstream README is kept as
[`README.upstream.md`](README.upstream.md).

This is GPLv3 software, like the code it builds on. See [`COPYING`](COPYING).

---

## English

### What's changed

- **New audio driver** (`audio/drivers/ps4_audio.c`, registered as `ps4`).
  The stock port paced audio through a blocking driver that ran near buffer
  underrun, so a game like KOF '98 crackled and lagged. The new driver uses a
  ring buffer sized from `audio_latency`, a dedicated worker thread, and
  dynamic-rate control, and runs with no underruns on the test content.
- **USB keyboard support** (`input/drivers/ps4_input.c`, build with
  `HAVE_KEYBOARD=1`). The retail `libSceKeyboard` exports only `sceKeyboard*`,
  so the functions are resolved at run time; the handle is opened once per
  process, held keys are tracked as a set so simultaneous presses (fighting-game
  motions) work, and keyboard binds are honoured in `ps4_input_state`.
- **CJK font fallback** (`gfx/drivers_font_renderer/stb_unicode.c`). The menu
  fonts cover only Latin; glyphs they lack are drawn from the system's
  `chinese-fallback-font.ttf`, so Chinese (and other CJK) playlist/game names
  render instead of showing as boxes.

Plus the OpenOrbis v0.5.x bring-up work the above depends on: a replacement
process entry point (`frontend/drivers/platform_orbis_crt.c`), a
`CLOCK_MONOTONIC` fixup for the FreeBSD kernel (`defines/oosdk_fixups.h`),
Piglet/`eglSwapInterval` handling, and `Makefile.ps4`.

### Building

Requires the [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
(tested with v0.5.2). Set `OO_PS4_TOOLCHAIN`, then:

```sh
# menu / frontend eboot
make -f Makefile.ps4 -j

# with keyboard support, linking a static libretro core archive
make -f Makefile.ps4 -j HAVE_KEYBOARD=1 CORE=/path/to/<core>_libretro.a
```

Each `*_libretro_ps4.self` core is a whole RetroArch+core `fself` launched via
`sceSystemServiceLoadExec`. The build embeds the standard PS4 system-privilege
`AUTH_INFO` (a widely published homebrew value, in `Makefile.ps4`) and PAID
`0x3100000000000002`; link `libc.prx` and `libSceFios2.prx`, and do not link
`ScePosix`. Packaging as a title (so cores can reach `app0/libc.prx`) is left to
your own `.pkg` flow.

Build artifacts (`eboot.bin`, `*.oelf`, `*.elf`) are git-ignored.

### Credits

[libretro/RetroArch](https://github.com/libretro/RetroArch) · the PS4 port by
[osirizx](https://github.com/osirizx) · the
[OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) toolchain.

---

## 中文

基于 [osirizx/retroarch](https://github.com/osirizx/retroarch)（[libretro/RetroArch](https://github.com/libretro/RetroArch)
1.8.8 的 PS4 移植版）的分支，用
[OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) 工具链重新编译，
加入三项改动，在 PS4 Pro（系统 12.02，GoldHEN）上开发并测试。原移植版的说明文档
保留为 [`README.upstream.md`](README.upstream.md)。本项目与所基于的代码一样采用
GPLv3，见 [`COPYING`](COPYING)。

### 改了什么

- **新的音频驱动**（`audio/drivers/ps4_audio.c`，注册名为 `ps4`）。原版移植用的
  阻塞式驱动长期贴着缓冲区下溢运行，像 KOF98 这类游戏会爆音、声音滞后。新驱动按
  `audio_latency` 计算环形缓冲区大小，用独立工作线程加动态采样率控制，测试内容下
  无下溢、无爆音、不滞后。
- **USB 键盘支持**（`input/drivers/ps4_input.c`，用 `HAVE_KEYBOARD=1` 编译）。
  零售版 `libSceKeyboard` 只导出 `sceKeyboard*`，函数在运行时解析；句柄每进程只
  打开一次，按下的键用集合跟踪，所以同时按多个键（格斗游戏搓招）也能正常识别，
  并在 `ps4_input_state` 中处理键盘绑定。
- **中日韩字体回退**（`gfx/drivers_font_renderer/stb_unicode.c`）。菜单自带字体
  只覆盖拉丁字符，缺的字改用系统的 `chinese-fallback-font.ttf` 绘制，于是列表和
  游戏名里的中文（及其他 CJK 文字）能正常显示，不再是方块。

以及上述功能依赖的 OpenOrbis v0.5.x 适配工作：替换的进程入口
（`frontend/drivers/platform_orbis_crt.c`）、针对 FreeBSD 内核的
`CLOCK_MONOTONIC` 修正（`defines/oosdk_fixups.h`）、Piglet/`eglSwapInterval`
处理，以及 `Makefile.ps4`。

### 编译

需要 [OpenOrbis PS4 工具链](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
（在 v0.5.2 上测试）。设好 `OO_PS4_TOOLCHAIN` 后：

```sh
# 菜单 / 前端 eboot
make -f Makefile.ps4 -j

# 带键盘支持，链接一个静态 libretro 核心库
make -f Makefile.ps4 -j HAVE_KEYBOARD=1 CORE=/path/to/<core>_libretro.a
```

每个 `*_libretro_ps4.self` 核心都是一个完整的 RetroArch+核心 `fself`，由
`sceSystemServiceLoadExec` 启动。编译时会嵌入标准的 PS4 系统权限 `AUTH_INFO`
（homebrew 圈内广泛公开的值，在 `Makefile.ps4` 里）和 PAID `0x3100000000000002`；
需链接 `libc.prx` 和 `libSceFios2.prx`，不要链接 `ScePosix`。打包成应用（让核心能
访问 `app0/libc.prx`）请用你自己的 `.pkg` 流程。编译产物（`eboot.bin`、`*.oelf`、
`*.elf`）已在 git 中忽略。

### 致谢

[libretro/RetroArch](https://github.com/libretro/RetroArch) · PS4 移植作者
[osirizx](https://github.com/osirizx) ·
[OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) 工具链。
