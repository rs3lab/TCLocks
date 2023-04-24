LDB := -lpq -pthread
CXXFLAGS := -g -Wall -MD -I$(LDB)

qe_objects := qe.o queryop.o between.o and.o  ../mkdb/args.o

all: qe

qe: $(qe_objects)
	g++ -o qe -L$(LDB) $(qe_objects) -ldb

.PHONY : clean
clean : 
	rm qe $(qe_objects) 
