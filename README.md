# 1coder

A modal text editor with vim keybindings. One binary, no config files, no plugin
manager, no scripting language.

## Install

Download from the [latest release](https://github.com/vasiltop/1coder/releases/latest):
Unpack it and put `1coder` somewhere on your `PATH`.

## Configuring it

There are no config files, and there is no plugin system. To configure the editor you must rebuild it from source.

## Building

```sh
git clone --recurse-submodules https://github.com/vasiltop/1coder
cmake -B build -S . && cmake --build build -j
```
