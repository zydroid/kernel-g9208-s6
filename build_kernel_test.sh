#!/bin/bash

export ARCH=arm64
export CROSS_COMPILE=/opt/toolchains/aarch64-linux-android-4.9/bin/aarch64-linux-android-

make ARCH=arm64 exynos7420-zerofltecmcc_defconfig
make ARCH=arm64