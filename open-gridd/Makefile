.PHONY: all test clean

all:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j$$(nproc)

test: all
	ctest --test-dir build --output-on-failure
	python3 -B tests/run_c_e2e.py build/c_e2e_tests
	python3 -B -m unittest discover -s tests -v

clean:
	rm -rf build build-pkg dist
