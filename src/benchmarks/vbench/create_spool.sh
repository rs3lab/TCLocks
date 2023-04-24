#!/bin/bash

#$1 -> partition type

spools="0 1 2 3 4 5 6 7 8 9 a b c d e f g h i j k l m n o p q r s t u v w x y z A B C D E F G H I J K L M N O P Q R S T U V W X Y Z"
pushd /tmp/mosbench/tmpfs/
sudo mkdir -p spool/input
pushd spool/input
count=2

for i in $spools;
do
    # first create a img file
    echo "creating $i img file"
    sudo dd if=/dev/zero of=${i}.img bs=1 count=0 seek=1G
    # now check for the /dev/loop$i
    if [[ ! -f /dev/loop$count ]]
    then
        # create the mknod
	echo "creating loop$count"
        sudo mknod -m660 /dev/loop$count b 7 8
        sudo chown root.disk /dev/loop$count
    fi
    # time for losetup
    echo "setting up using losetup"
    sudo losetup -d /dev/loop$count
    sudo losetup /dev/loop$count ${i}.img
    # time for mkfs
    echo "== creating partition $1 for $i =="
    case $1 in
        ext4|ext4_no_jnl)
            sudo mkfs.ext4 -F /dev/loop$count
            ;;
        btrfs|xfs)
            sudo mkfs.$1 -f /dev/loop$count
            ;;
		f2fs)
			sudo mkfs.$1 /dev/loop$count
			;;
    esac
    echo "mounting file system $1 using $i"
    sudo mkdir $i
    if [ "$1" == "ext4_no_jnl" ]
    then
        sudo tune2fs -O ^has_journal /dev/loop$count
        sudo mount -t ext4 /dev/loop$count /tmp/mosbench/tmpfs/spool/input/$i
    fi
    sudo mount -t $1 /dev/loop$count /tmp/mosbench/tmpfs/spool/input/$i
    sudo rm -rf $i/lost+found
    sudo chown sanidhya.sanidhya $i -R
    count=$(( $count + 1 ))
done


popd
sudo chown sanidhya.sanidhya spool -R
popd
