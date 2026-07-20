.PHONY: all build/llama_cpp install clean

all: install

build/llama_cpp.stamp:
	mkdir -p build/llama_cpp
	cd build/llama_cpp && cmake ../../../vendor/llama.cpp -DCMAKE_BUILD_TYPE=Release -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_UI=OFF -DLLAMA_OPENSSL=OFF -DBUILD_SHARED_LIBS=ON -DGGML_NATIVE=ON
	touch build/llama_cpp.stamp

build/llama_cpp: build/llama_cpp.stamp
	cd build/llama_cpp && make -j $(nproc) llama-common

build/llama_cpp_install.stamp: build/llama_cpp
	find build/llama_cpp -iname '*.so' -exec cp '{}' ../build/ \; 
	cd ../build && ln -sfT libllama.so ../build/libllama.so.0
	find build/llama_cpp -iname '*.so' -exec cp '{}' build/ \; 
	cd build && ln -sfT libllama.so libllama.so.0
	touch build/llama_cpp_install.stamp

install: build/llama_cpp_install.stamp
	echo "done"

clean:
	rm -rf build/llama_cpp*

