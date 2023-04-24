#!/bin/bash

qemu-system-x86_64 --enable-kvm \
	-m 32G \
	-device virtio-serial-pci,id=virtio-serial0,bus=pci.0,addr=0x6 \
	-drive file=$1,cache=none,if=none,id=drive-virtio-disk0,format=raw \
	-device virtio-blk-pci,scsi=off,bus=pci.0,addr=0x7,drive=drive-virtio-disk0,id=virtio-disk0 \
	-netdev user,id=hostnet0,hostfwd=tcp::$4-:22 \
	-device virtio-net-pci,netdev=hostnet0,id=net0,bus=pci.0,addr=0x3 \
	-vnc :$6 \
	-cpu host \
	-qmp tcp:localhost:$5,server,nowait \
	-smp cores=$2,threads=1,sockets=$3 \
	-realtime mlock=on &
	#-kernel /home/sanidhya/research/linux/arch/x86/boot/bzImage \
	#--initrd /boot/initrd.img-$(uname -r) \
	#-append "root=/dev/vda1" \

# start the vm
cores=$(($2 * $3))
sleep 2
python /home/sanidhya/bench/vm-scalability/bench/vm-scripts/pin-vcpu.py $5 ${cores}
