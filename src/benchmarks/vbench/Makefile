TARGETS=exim gmake psearchy
NJOP := ${shell nproc}

all: $(addprefix all-,$(TARGETS))
clean: $(addprefix clean-,$(TARGETS))

.PHONY: always

all-qemu: always
	$(MAKE) -C ../src qemu

all-libdb: always
	$(MAKE) -j${NJOP} -C libdb all

clean-libdb: always
	$(MAKE) -j${NJOP} -C libdb clean

all-exim: all-libdb always
	$(MAKE) -j${NJOP} -C exim all

clean-exim: always
	$(MAKE) -j${NJOP} -C exim clean
	$(MAKE) -j${NJOP} -C exim exim-clean

clean-gmake: always

all-gmake: always

all-rocksdb: always
	$(MAKE) -j${NJOP} -C rocksdb DEBUG_LEVEL=0

clean-rocksdb: always
	$(MAKE) -C rocksdb clean
 
all-psearchy: always
	$(MAKE) -C psearchy/mkdb all

clean-psearchy: always
	$(MAKE) -C psearchy/mkdb clean
 
.PHONY: bench
bench:
	python config.py
