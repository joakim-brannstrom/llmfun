.PHONY: all build/llama_cpp clean

all: build/llama_cpp

build/llama_cpp.stamp:
	mkdir -p build/llama_cpp
	cd build/llama_cpp && cmake ../../vendor/llama.cpp -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_UI=OFF -DLLAMA_OPENSSL=OFF -DBUILD_SHARED_LIBS=ON -DGGML_NATIVE=ON
	touch build/llama_cpp.stamp

build/llama_cpp: build/llama_cpp.stamp
	cd build/llama_cpp && make -j $(nproc)

clean:
	rm -rf build/llama_cpp*

