#!/bin/bash

mkdir -p lib
cd lib

git clone https://github.com/g-truc/glm.git glm_sources

cd glm_sources

mkdir -p build
cmake -DGLM_BUILD_TESTS=OFF -DBUILD_SHARED_LIBS=OFF -B build .
cmake --build build -- all

cd ..
cp -r glm_sources/glm .
cp -r glm_sources/build/glm/libglm.a .
rm -rf glm_sources

echo "GLM installed successfully"
