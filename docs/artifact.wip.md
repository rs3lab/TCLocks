---
title: Artifact Evaluation Guide
---

# Table of Contents
---

- [Overview](#overview)
	- [TCLocks](#tclocks)
	- [Environment](#environment)
- [Getting started instructions](#getting-started-instructions)
- [Running Experiments](#running-experiments)
	- [Micro-benchmark](#micro-benchmark)
	- [Macro-benchmark](#macro-benchmark)
	- [Nano-benchmark](#nano-benchmark)
	- [Userspace-benchmark](#userspace-benchmark)
	- [Generate data](#generate-data)
- [How to create the disk image](#how-to-create-the-disk-image)


# Overview
---

## TCLocks

[TCLocks repository](https://github.com/rs3lab/TCLocks) contains the source code
of TCLocks and the benchmarks we used in the paper.
This is the directory structure of the TCLocks repo.

	TCLocks
	  ├── src                  : TCLocks source code
	  |    ├── concord
	  |    └── kpatch
	  ├── benchmarks           : benchmark sets used in the paper
	  |    ├── will-it-scale
	  |    ├── fxmark
	  |    ├── metis
	  |    ├── levelDB
	  |    ├── xDir-rename
	  |    └── lockstat
	  └── scripts              : script to run qemu

## Environment
The experiments in this artifact are designed to run on a *NUMA* machine with
multiple sockets and tens of CPUs.
The result shown in the paper is evaluated on a 8-socket, 224-core machine
equipped with Intel Xeon Platinum 8276L CPUs. The machine runs Ubuntu 20.04 with
Linux 5.4.0 and hyperthreading is disabled.


# Getting Started Instructions
---

The easiest way to reproduce TCLocks is to use QEMU with our ready-to-use disk
image. In this guide, we introduce how to quickly setup an environment to
run TCLocks using the disk image.
There is also a section guides [how to create the disk
image](#how-to-create-the-disk-image) from scratch.


## 1. Clone the TCLocks repo
---
Download the compresssed disk image
[here](update-link).
Once you finish downloading the file, uncompress it using following command.

	$ pv vm.img.xz | unxz -T <num_threads> > ubuntu-20.04.img


In addition, clone the TCLocks repo.

	$ git clone https://github.com/rs3lab/TCLocks.git


## 2. Start a QEMU virtual machine
---

Start qemu with following script.
The script is also available under `TCLocks/scripts/run-vm.sh`.

> [Dependency]
> You may need following commands to install qemu and execute it without sudo.
>
	$ sudo apt install qemu-kvm
	$ sudo chmod 666 /dev/kvm

	$ ./qemu-system-x86_64 \
		--enable-kvm \
		-m 128G \
		-cpu host \
		-smp cores=28,threads=1,sockets=8 \
		-numa node,nodeid=0,mem=16G,cpus=0-27 \
		-numa node,nodeid=1,mem=16G,cpus=28-55 \
		-numa node,nodeid=2,mem=16G,cpus=56-83 \
		-numa node,nodeid=3,mem=16G,cpus=84-111 \
		-numa node,nodeid=4,mem=16G,cpus=112-139 \
		-numa node,nodeid=5,mem=16G,cpus=140-167 \
		-numa node,nodeid=6,mem=16G,cpus=168-195 \
		-numa node,nodeid=7,mem=16G,cpus=196-223 \
		-drive file=/path/to/downloaded/syncord-vm.img,format=raw \
		-nographic \
		-overcommit mem-lock=off
		-qmp tcp:127.0.0.1:5555,server,nowait \
		-device virtio-serial-pci,id=virtio-serial0,bus=pci.0,addr=0x6 \
		-device virtio-net-pci,netdev=hostnet0,id=net0,bus=pci.0,addr=0x3 \
		-netdev user,id=hostnet0,hostfwd=tcp::4444-:22 \


The command starts a virtual machine with 128G of memory and 8 NUMA sockets each
equipped with 28 cores, results in 224 cores in total. Please adjust the
numbers and path to the vm image for your environment.
The script opens port `4444` for ssh and `5555` for qmp.

When you face the grub menu, just wait for 5 seconds, then the guest will be
start with default `5.14.16-stock` kernel.

The provided disk image contains one 30GB partition holding Ubuntu 20.04 and
one 20GB partition for experiments.
There is single user `ubuntu` with password `ubuntu`, who has sudo power.
Use port 4444 to ssh into the machine.

	$ ssh syncord@localhost -p 4444

The home directory already contains `TCLocks` repo.

## 3. Pin vCPU to physical cores
---

The port 5555 is for `qmp` which allows us to observe NUMA effect with vCPU by
pinning each vCPU to physical cores. __This step must be done__ before measuring numbers.
Run the `pin-vcpu.py` script to pin the cores. Here, `num_vm_cores` is 224 with
above example.

	$ python2 ./TCLocks/scripts/pin-vcpu.py 5555 <num_vm_cores>

> [Dependency] Since the `pin-vcpu.py` use python2.7, you might need following commands to
> install pip for python2.7 and psutil package.
> If you didn't change the kvm permission in above [step](#2-start-a-qemu-virtual-machine),
> run following commands and above `pin-vcpu.py` with `sudo`

	$ sudo python2 ./TCLocks/scripts/get-pip.py
	$ python2 -m pip install psutil


Once you start the VM, let's check you're on the right kernel version.

	(guest)$ uname -r

If you see `5.14.16-stock`, you're all set and now it's time to use TCLocks!


## 4. Build kernel for all four locks.
---

	$ ./TCLocks/scripts/build-all-kernel.sh

## 5. Test the setup
---
	
	$ ./TCLocks/scripts/run-test.sh


# Running Experiments
---

Main scripts are under `./TCLocks/scripts/`. You can run all of the steps below using :
	
	$ ./TCLocks/scripts/run-all.sh

## Micro-benchmark 
(Figure 6)

	$ ./TCLocks/scripts/run-micro-benchmark.sh

## Macro-benchmark 
(Figure 7)

	$ ./TCLocks/scripts/run-macro-benchmark.sh

## Nano-benchmark 
(Figure 8)

	$ ./TCLocks/scripts/run-nano-benchmark.sh

## Userspace-benchmark 
(Figure 9)

	$ ./TCLocks/scripts/run-userspace-benchmark.sh

## Generate data
	
	$ ./TCLocks/scripts/run-parse-script.sh

# How to create the disk image
---

## 1. Create a bootable image

First, download a ubuntu 20.04 LTS image ([link](https://releases.ubuntu.com/20.04/)).

	$ wget https://releases.ubuntu.com/20.04/ubuntu-20.04.4-live-server-amd64.iso

Create a storage image.

	$ qemu-img create ubuntu-20.04.img 30G

Start QEMU and use the downloaded iso image as a booting disk.

	$ ./qemu-system-x86_64 \
		--enable-kvm \
		-m 128G \
		-cpu host \
		-smp cores=28,threads=1,sockets=8 \
		-numa node,nodeid=0,mem=16G,cpus=0-27 \
		-numa node,nodeid=1,mem=16G,cpus=28-55 \
		-numa node,nodeid=2,mem=16G,cpus=56-83 \
		-numa node,nodeid=3,mem=16G,cpus=84-111 \
		-numa node,nodeid=4,mem=16G,cpus=112-139 \
		-numa node,nodeid=5,mem=16G,cpus=140-167 \
		-numa node,nodeid=6,mem=16G,cpus=168-195 \
		-numa node,nodeid=7,mem=16G,cpus=196-223 \
		-drive file=/path/to/created/ubuntu-20.04.img,format=raw \
		-cdrom /path/to/downloaded/ubuntu-20.04.4-live-server-amd64.iso \


Please make sure to have multiple sockets and enough number of cores to evaluate
lock scalability on NUMA machine.

If X11 connection is there, you'll see the QEMU GUI popup window to install
ubuntu. Install ubuntu server with OpenSSH package and disabled LVM.
Then login to the installed user.
If you don't have X11 connection, please refer this
[link](https://github.com/XieGuochao/cloud-image-builder) to setup the image.

Open `/etc/default/grub` and update `GRUB_CMDLINE_LINUX_DEFAULT="console=ttyS0"`.
This will print initial booting messages to the console on the start of the
guest vm.
Then, run the following commands to apply the change and shutdown the guest
machine.

	(guest)$ sudo update-grub
	(guest)$ sudo shutdown -h now

Now, you can start your QEMU without the iso file and graphic.

	$ ./qemu-system-x86_64 \
		--enable-kvm \
		-m 128G \
		-cpu host \
		-smp cores=28,threads=1,sockets=8 \
		-numa node,nodeid=0,mem=16G,cpus=0-27 \
		-numa node,nodeid=1,mem=16G,cpus=28-55 \
		-numa node,nodeid=2,mem=16G,cpus=56-83 \
		-numa node,nodeid=3,mem=16G,cpus=84-111 \
		-numa node,nodeid=4,mem=16G,cpus=112-139 \
		-numa node,nodeid=5,mem=16G,cpus=140-167 \
		-numa node,nodeid=6,mem=16G,cpus=168-195 \
		-numa node,nodeid=7,mem=16G,cpus=196-223 \
		-drive file=/path/to/created/ubuntu-20.04.img,format=raw \
		-nographic \
		-overcommit mem-lock=off \
		-device virtio-serial-pci,id=virtio-serial0,bus=pci.0,addr=0x6 \
		-device virtio-net-pci,netdev=hostnet0,id=net0,bus=pci.0,addr=0x3 \
		-netdev user,id=hostnet0,hostfwd=tcp::4444-:22 \
		-qmp tcp:127.0.0.1:5555,server,nowait \


## 2. Install custom kernel
---

To use custom kernel, you have two options.

First, compile the kernel and install it within the guest vm.\\
Or, compile the kernel in the host machine and pass it to the qemu via `-kernel`
option.

The second option is more convenient for frequent kernel changes, but it still
requires one time static install for kernel modules.
TCLocks-linux has three lock implementation all based on linux kernel v5.4, so you
can reuse kernel modules across the three branches (stock, cna, shfllock) once
installed.

To build and install a kernel inside the vm, first clone the TCLocks-linux repo
and resolve dependencies.

	(guest)$ git clone -b stock https://github.com/rs3lab/TCLocks-linux.git
	(guest)$ cd TCLocks-linux

> [Dependency]
> You may need following commands to build linux
>
	(guest) $ sudo apt-get install build-essential libncurses5 libncurses5-dev bin86 \
		kernel-package libssl-dev bison flex libelf-dev
>
> [Dependency]
> In addition, please make sure you're using gcc-7.
>
	(guest) $ gcc -v
	...
	gcc version 7.5.0 (Ubuntu 7.5.0-6ubuntu2)


Before start compilation, please make sure `CONFIG_PARAVIRT_SPINLOCKS` is not
set in your `.config` file.

	(guest)$ make -j <num_threads>
	(guest)$ sudo make modules_install
	(guest)$ sudo make install
	(guest)$ sudo shutdown -h now		# Install complete. Shut down guest machine

Now, in the host machine, you can choose a branch you want to use and then start
a qemu with that lock implementation. Please make sure you're using gcc-7 here
too.

	$ gcc -v
	...
	gcc version 7.5.0

	$ git clone https://github.com/rs3lab/TCLocks-linux.git
	$ cd ~/TCLocks-linux
	$ git checkout -t origin/stock		# or select origin/cna, origin/shfllock
	$ make -j <num_threads>

	$ ./qemu-system-x86_64 \
		--enable-kvm \
		-m 128G \
		-cpu host \
		-smp cores=28,threads=1,sockets=8 \
		-numa node,nodeid=0,mem=16G,cpus=0-27 \
		-numa node,nodeid=1,mem=16G,cpus=28-55 \
		-numa node,nodeid=2,mem=16G,cpus=56-83 \
		-numa node,nodeid=3,mem=16G,cpus=84-111 \
		-numa node,nodeid=4,mem=16G,cpus=112-139 \
		-numa node,nodeid=5,mem=16G,cpus=140-167 \
		-numa node,nodeid=6,mem=16G,cpus=168-195 \
		-numa node,nodeid=7,mem=16G,cpus=196-223 \
		-drive file=/path/to/created/ubuntu-20.04.img,format=raw \
		-nographic \
		-overcommit mem-lock=off \
		-device virtio-serial-pci,id=virtio-serial0,bus=pci.0,addr=0x6 \
		-device virtio-net-pci,netdev=hostnet0,id=net0,bus=pci.0,addr=0x3 \
		-netdev user,id=hostnet0,hostfwd=tcp::4444-:22 \
		-qmp tcp:127.0.0.1:5555,server,nowait \
		-kernel ~/TCLocks/src/kernel/linux-5.14.16/arch/x86/boot/bzImage \
		-append "root=/dev/sda2 console=ttyS0" \

The `uname -r` command confirms that the current guest VM is booted using the
custom kernel.

	(guest)$ uname -r
	5.14.9-stock

## 3. Setup TCLocks

Inside the guest VM, clone the [TCLocks](https://github.com/rs3lab/SynCord) and
[TCLocks-linux](https://github.com/rs3lab/SynCord-linux) repositories.

install dependencies and build:

	$ sudo apt install python3-pip clang llvm
	$ pip3 install gitpython
	$ cd ~/TCLocks/src/kpatch && make
