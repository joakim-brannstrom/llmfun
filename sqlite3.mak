CC?=	/usr/bin/cc

all: build/sqlite3.stamp

# AVX kernels are chosen at run time inside sqlite-vec, so the same
# object is safe on pre-AVX hosts; only the define is needed.
ifneq ($(filter x86_64 amd64,$(shell uname -m)),)
AVX_FLAGS := -DSQLITE_VEC_ENABLE_AVX
endif

build/sqlite3.stamp:
	mkdir -p build
	$(CC) -c -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_COLUMN_METADATA -DSQLITE_ENABLE_UNLOCK_NOTIFY -DSQLITE_ENABLE_FTS5 vendor/sqlite3/sqlite3.c -o build/sqlite3.o
	$(CC) -c -O2 $(AVX_FLAGS) -Ivendor/sqlite3 -DSQLITE_CORE -DSQLITE_VEC_STATIC vendor/sqlite3-vec/sqlite-vec.c -o build/sqlite-vec.o
	touch build/sqlite3.stamp

clean:
	rm -f build/sqlite3-vec.o build/sqlite3.o build/sqlite3.stamp
