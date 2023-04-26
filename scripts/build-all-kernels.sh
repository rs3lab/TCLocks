

root_dir=`pwd`
mkdir tmp

cd tmp

git clone https://github.com/rs3lab/TCLocks.git

#Build TCLock bzImage

cd TCLocks/src/kernel/linux-5.14.16/
make clean -j`nproc`
make -j `nproc`
mv arch/x86/boot/bzImage ${root_dir}/tclocks.bzImage
git checkout -- .

#Build stock bzImage

git checkout stock
make clean -j`nproc`
make -j `nproc`
mv arch/x86/boot/bzImage ${root_dir}/stock.bzImage
git checkout -- .

#Build cna bzImage

git checkout cna
make clean -j`nproc`
make -j `nproc`
mv arch/x86/boot/bzImage ${root_dir}/cna.bzImage
git checkout -- .

#Build shfllock bzImage

git checkout shfllock
make clean -j`nproc`
make -j `nproc`
mv arch/x86/boot/bzImage ${root_dir}/shfllock.bzImage
git checkout -- .

cd ${root_dir}

rm -r tmp/
