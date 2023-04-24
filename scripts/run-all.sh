
if [ ! -f "stock.bzImage" ]; then
	./build-all-kernels.sh
fi

./run-micro-benchmark.sh

./run-macro-benchmark.sh

./run-nano-benchmark.sh

./run-userspace-benchmark.sh
