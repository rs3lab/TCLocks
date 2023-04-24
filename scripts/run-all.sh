
if [ ! -f "stock.bzImage" ]; then
	./build-all-kernels.sh
fi

rm -f completion.log

touch completion.log

./run-nano-benchmark.sh

./run-userspace-benchmark.sh

./run-macro-benchmark.sh

./run-micro-benchmark.sh
