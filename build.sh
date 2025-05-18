KERNEL=kernel_2712
CONFIG_LOCALVERSION="-v1-Littlenine_Kernel"
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- bcm2712_defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 Image modules dtbs
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- INSTALL_MOD_PATH=out modules_install