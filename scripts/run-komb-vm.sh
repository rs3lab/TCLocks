#!/usr/bin/env bash

#sudo apt-get install -y cloud-image-utils qemu

# This is already in qcow2 format.
img=tclocks-vm.img

if [ ! -f "${img}" ]; then
	wget "image"
fi

KERNEL=stock.bzImage

if [[ $1 == "shfllock" ]] || [[ $1 == "tclocks" ]] || [[ $1 == "cna" ]]; then
	KERNEL=${1}.bzImage
fi

sudo qemu-system-x86_64 \
	-nographic \
	--enable-kvm \
	-drive "file=${img},format=qcow2" \
	-m 128G \
	-cpu host \
	-object memory-backend-ram,size=16G,id=nr0 \
	-object memory-backend-ram,size=16G,id=nr1 \
	-object memory-backend-ram,size=16G,id=nr2 \
	-object memory-backend-ram,size=16G,id=nr3 \
	-object memory-backend-ram,size=16G,id=nr4 \
	-object memory-backend-ram,size=16G,id=nr5 \
	-object memory-backend-ram,size=16G,id=nr6 \
	-object memory-backend-ram,size=16G,id=nr7 \
	-numa node,nodeid=0,memdev=nr0,cpus=0-27 \
	-numa node,nodeid=1,memdev=nr1,cpus=28-55 \
	-numa node,nodeid=2,memdev=nr2,cpus=56-83 \
	-numa node,nodeid=3,memdev=nr3,cpus=84-111 \
	-numa node,nodeid=4,memdev=nr4,cpus=112-139 \
	-numa node,nodeid=5,memdev=nr5,cpus=140-167 \
	-numa node,nodeid=6,memdev=nr6,cpus=168-195 \
	-numa node,nodeid=7,memdev=nr7,cpus=196-223 \
	-smp cores=28,threads=1,sockets=8 \
	-device e1000,netdev=net0 \
	-qmp tcp:127.0.0.1:5556,server,nowait \
	-netdev user,id=net0,hostfwd=tcp::4446-:22 -kernel ${KERNEL} \
	-append "root=/dev/sda1 console=ttyS0 nokaslr maxcpus=250 nr_cpus=250 possible_cpus=250 panic=1 numa_spinlock=on" \
	-initrd komb-ramdisk.img \
	-overcommit mem-lock=off \
