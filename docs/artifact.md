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
	  |    ├── kernel
	  |    |    ├── linux-5.14.16   : Kernel with TCLocks
	  |    |    ├── rcuht	 	: Hash-table nano-benchmark
	  |    └── userspace	   : Userspace implementation of TCLocks
	  |    |    ├── litl 		
	  |    └── defaults.sh     : Default parameters used for benchmarks
	  |    ├── benchmarks           : benchmark sets used in the paper
	  |    |    ├── will-it-scale   
	  |    |    ├── fxmark
	  |    |    ├── vbench
	  |    |    ├── leveldb-1.20
	  └── scripts              : script to run experiments

Different branches contain the source code of Linux 5.14.16 with different locks. 

## Environment
The experiments in this artifact are designed to run on a machine with
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
Until otherwise noted, all the steps are executed on the host machine.

## 1. Clone the TCLocks repo
---

Clone the TCLocks repo.

	$ git clone https://github.com/rs3lab/TCLocks.git
	$ cd TCLocks

Download the compresssed disk image
[here](https://zenodo.org/record/7860633/files/tclocks-vm.img.gz?download=1).
Once you finish downloading the file, uncompress it using following command and move it to the scripts repo.

	$ wget https://zenodo.org/record/7860633/files/tclocks-vm.img.gz?download=1
	$ sudo apt install pigz
	$ unpigz tclocks-vm.img.gz
	$ mv tclocks-vm.img TCLocks/scripts/

## 2. Build kernel for all four locks.
---

Install gcc-9. Add the following line to /etc/apt/sources.list

	$ "deb [arch=amd64] http://archive.ubuntu.com/ubuntu focal main universe"

Then install gcc using the following command :

	$ sudo apt update && sudo apt install gcc-9	
	$ sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-9 40

Choose gcc-9 when running the following command :

	$ sudo update-alternatives --config gcc

Check if gcc version is correct :

	$ gcc -v

Install tools required to build a kernel.
	
	$ sudo apt-get install build-essential libncurses5 libncurses5-dev bin86 \
                kernel-package libssl-dev bison flex libelf-dev

The following script builds bzImage for kernels for all four locks: stock, cna, shfllock and tclocks

	$ cd TCLocks/scripts/
	$ ./build-all-kernel.sh

## 3. Start a QEMU virtual machine
---

Start qemu with script under `TCLocks/scripts/run-vm.sh`.

The command starts a virtual machine with 128G of memory and 8 NUMA sockets each
equipped with 28 cores, results in 224 cores in total. 
**Adjust the number of cores and sockets based on the host machine.**
Its preferable to have the same configuration as the host machine.
Update the path to the vm image for your environment.
The script opens port `4444` for ssh and `5555` for qmp.

The guest will be start with default `5.14.16-stock` kernel.

The provided disk image contains one 50GB partition holding Ubuntu 20.04 and TCLocks repo.
There is single user `ubuntu` with password `ubuntu`, who has sudo power.
Use port 4444 to ssh into the machine.

	$ ssh ubuntu@localhost -p 4444

The home directory already contains `TCLocks` repo.

## 4. Setup passwordless-ssh to QEMU virtual machine

Script provided for running experiments require passwordless-ssh to the virtual machine. 
You can set it up using the following command :

	$ ssh-keygen -t ed25519

Assuming the public key is in ~/.ssh/id_ed25519.pub :

	$ ssh-copy-id -i ~/.ssh/id_ed25519.pub -p 4444 ubuntu@localhost

## 5. Pin vCPU to physical cores
---

The port 5555 is for `qmp` which allows us to observe NUMA effect with vCPU by
pinning each vCPU to physical cores. This step must be done before measuring numbers.
This step is automatically done by the included experiment script.
Run the `pin-vcpu.py` script to pin the cores. Here, `num_vm_cores` is 224 with
above example. 

Install `qmp` and `psutils` on the host machine to pin the vCPUs.

	$ pip install qmp
	$ pip install psutils
	$ sudo ./TCLocks/scripts/pin-vcpu.py 5555 <num_vm_cores>

Once you start the VM, let's check you're on the right kernel version.

	(guest)$ uname -r

If you see `5.14.16-stock`, you're all set and now it's time to use TCLocks!

## 6. Test the setup
---

SSH into the VM and update the defaults.sh file in the `~/TCLocks/src` directory.

1. Set the `cores` and `mutex_cores` to 1.
2. Set the `python_env_cores` to 28.
3. Set the `runtime` to 1.

Shutdown the VM and execute in the host machine:
	
	$ cd TCLocks/scripts
	$ ./run-all.sh


# Running Experiments
---

Main scripts are under `./TCLocks/scripts/`. You can run all of the steps below using :
	
	$ cd TCLocks/scripts
	$ ./run-all.sh

**Before running the experiments, SSH into the VM and update the defaults.sh file in the `~/TCLocks/src` directory.**

1. Set the `cores` and `python_env_cores` to a list of CPUs upto the maximum number of CPUs in the VM. 
For example, if the VM has 28 cores.

	cores=(1 2 4 8 12 16 20 28)
	python_env_cores='[1,2,4,8,12,16,20,28]'

2. Set the `mutex_cores` to a list of CPUs upto 4x the maximum number of CPUs in the VM. 
For oversubscription ( > maximum number of CPUs ), a few core counts are enough to validate.
For example, if the VM has 28 cores.

	mutex_cores=(1 2 4 8 12 16 20 28 56 84 112)

2. Set the `ncores` to the maximum number of CPUs in the VM.

	ncores=28

3. Set the `runtime` to 30 seconds.
	
	runtime=30

## Micro-benchmark 
(Figure 6)

	$ cd TCLocks/scripts
	$ ./run-micro-benchmark.sh

**Expected Results**:

* The performance for TCLocks at 2-8 cores will be slightly worse than stock.
* The performance for TCLocks above 8 cores will be better that stock / CNA / Shfllock.
* The performance will be similar at core count above 12 cores.

## Macro-benchmark 
(Figure 7)

	$ cd TCLocks/scripts
	$ ./run-macro-benchmark.sh

**Expected Results**:

* The performance for TCLocks at 2-8 cores will be similar to stock.
* The performance for TCLocks above 8 cores will be better than stock / CNA / Shfllock.

## Nano-benchmark 
(Figure 8)

	$ cd TCLocks/scripts
	$ ./run-nano-benchmark.sh

**Expected Results**:

* The performance for TCLocks at 2-4 cores will be similar or lower than stock for spinlock (Figure 8(a)).
* The performance for TCLocks for Spinlock (Figure 8(a)) / Mutex (Figure 8(c)) / RWSem (Figure 8(d),(e)) above 8 cores will be better that stock / CNA / Shfllock.
* The performance will improve after adding optimization like NUMA, Prefetching and Waiter to Waiter Jump (Figure 8(f)). 
* The performance will improve when prefetching 4/6 stack cache lines (Figure 8(g)).
* The performance will improve with increasing batch size (Figure 8(h)).

## Userspace-benchmark 
(Figure 9)

	$ cd TCLocks/scripts
	$ ./run-userspace-benchmark.sh

**Expected Results**:

* The performance for TCLocks will be better than stock / Mutex / RWSem.

## Generate data
	
	$ cd TCLocks/scripts
	$ ./run-parse-script.sh

# How to create the disk image
---

## 1. Create a bootable image

First, download a ubuntu 20.04 LTS image ([link](https://releases.ubuntu.com/20.04/)).

	$ wget https://releases.ubuntu.com/20.04/ubuntu-20.04.4-live-server-amd64.iso

Create a storage image.

	$ qemu-img create ubuntu-20.04.img 50G

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
TCLocks-linux has three lock implementation all based on linux kernel v5.14.16, so you
can reuse kernel modules across the three branches (stock, cna, shfllock) once
installed.

To build and install a kernel inside the vm, first clone the TCLocks-linux repo
and resolve dependencies.

	(guest)$ git clone -b stock https://github.com/rs3lab/TCLocks.git
	(guest)$ cd TCLocks

> [Dependency]
> You may need following commands to build linux
>
	(guest) $ sudo apt-get install build-essential libncurses5 libncurses5-dev bin86 \
		kernel-package libssl-dev bison flex libelf-dev
>
> [Dependency]
> In addition, please make sure you're using gcc-9.
>
	(guest) $ gcc -v
	...
	gcc version 9.5.0


Before start compilation, please make sure `CONFIG_PARAVIRT_SPINLOCKS` is not
set in your `.config` file.

	(guest)$ make -j <num_threads>
	(guest)$ sudo make modules_install
	(guest)$ sudo make install
	(guest)$ sudo shutdown -h now		# Install complete. Shut down guest machine

Now, in the host machine, you can choose a branch you want to use and then start
a qemu with that lock implementation. Please make sure you're using gcc-9 here
too.

	$ gcc -v
	...
	gcc version 9.5.0

	$ git clone https://github.com/rs3lab/TCLocks.git
	$ cd ~/TCLocks
	$ git checkout -t origin/stock		# or select origin/cna, origin/shfllock, origin/master (For TCLocks)
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
	5.14.16-stock

## 3. Setup TCLocks

Inside the guest VM, clone the [TCLocks](https://github.com/rs3lab/TCLocks) repo

