#!/usr/bin/env bash

g++ -std=c++20 -Ofast -o comparator comparator.cpp

./comparator -A "test/A" -B "test/B" -p 30
