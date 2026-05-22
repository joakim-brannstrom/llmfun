CC?=	/usr/bin/cc

all: build/sqlite3.stamp

build/sqlite3.stamp:
	mkdir -p build
	$(CC) -c -O2 -DSQLITE_ENABLE_COLUMN_METADATA -DSQLITE_ENABLE_UNLOCK_NOTIFY vendor/sqlite3/sqlite3.c -o build/sqlite3.o
	$(CC) -c -O2 -Ivendor/sqlite3 -DSQLITE_CORE -DSQLITE_VEC_STATIC vendor/sqlite3-vec/sqlite-vec.c -o build/sqlite-vec.o
	touch build/sqlite3.stamp

clean:
	rm -f build/sqlite3-vec.o build/sqlite3.o build/sqlite3.stamp
