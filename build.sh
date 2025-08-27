#!/bin/bash -ex

# This script replicates the fuzzbench build process for the harfbuzz_hb-shape-fuzzer_17863b benchmark,
# but uses a custom coverage target instead of the default libFuzzer.

# 1. Set up paths
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
HARFBUZZ_DIR="$SCRIPT_DIR/harfbuzz"
COVALYZER_LIB="$SCRIPT_DIR/libCovalyzer.a"
COVALYZER_SRC="$SCRIPT_DIR/CovalyzerTarget.c"
HARFBUZZ_COMMIT="17863BD16BC82C54FB68627CBF1E65702693DD09"

# 2. Build CovalyzerTarget.c into a static library
echo "Building Covalyzer library from $COVALYZER_SRC..."
clang -c "$COVALYZER_SRC" -o "$SCRIPT_DIR/CovalyzerTarget.o"
ar rcs "$COVALYZER_LIB" "$SCRIPT_DIR/CovalyzerTarget.o"
rm "$SCRIPT_DIR/CovalyzerTarget.o"
echo "Covalyzer library built at $COVALYZER_LIB"

# 3. Clone HarfBuzz if it doesn't exist and checkout the correct commit
if [ ! -d "$HARFBUZZ_DIR" ]; then
    echo "Cloning HarfBuzz..."
    git clone https://github.com/harfbuzz/harfbuzz.git "$HARFBUZZ_DIR"
fi
cd "$HARFBUZZ_DIR"
git checkout "$HARFBUZZ_COMMIT"
cd "$SCRIPT_DIR"

# 4. Set up build environment (mirroring fuzzbench)
export CC=clang
export CXX=clang++
# These flags are taken directly from the fuzzbench build.sh for harfbuzz
export CFLAGS="-fprofile-instr-generate -fcoverage-mapping -gline-tables-only -fno-sanitize=vptr -DHB_NO_VISIBILITY -DHB_NO_PRAGMA_GCC_DIAGNOSTIC -Wno-cast-function-type-strict -Wno-incompatible-function-pointer-types-strict"
export CXXFLAGS="-fprofile-instr-generate -fcoverage-mapping -gline-tables-only -fno-sanitize=vptr -DHB_NO_VISIBILITY -DHB_NO_PRAGMA_GCC_DIAGNOSTIC -Wno-cast-function-type-strict -Wno-incompatible-function-pointer-types-strict"
# This is the crucial part: we replace the default libFuzzer with our own library.
export LIB_FUZZING_ENGINE="$COVALYZER_LIB" 

# 5. Configure and build HarfBuzz
echo "Configuring and building HarfBuzz..."
build_dir=$HARFBUZZ_DIR/build
rm -rf "$build_dir"
mkdir -p "$build_dir"

meson --default-library=static --wrap-mode=nodownload \
      -Dexperimental_api=true \
      -Dfuzzer_ldflags="$LIB_FUZZING_ENGINE" \
      "$HARFBUZZ_DIR" "$build_dir" || (cat "$build_dir/meson-logs/meson-log.txt" && false)

ninja -v -j$(nproc) -C "$build_dir" test/fuzzing/hb-shape-fuzzer

# 6. Copy the final binary to the CCovalyzer directory
echo "Build complete. Copying binary to $SCRIPT_DIR"
mv "$build_dir/test/fuzzing/hb-shape-fuzzer" "$SCRIPT_DIR/"

echo "Successfully built hb-shape-fuzzer with custom coverage target."
echo "The binary is located at: $SCRIPT_DIR/hb-shape-fuzzer"
