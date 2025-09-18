#!/bin/bash

version="10"
dirs=("i-fb-a" "i-sf-a" "i-fb-q" "i-sf-q"
      "s-fb-a" "s-sf-a" "s-sl-a"
      "s-fb-q" "s-sf-q" "s-sl-q")

for dir in "${dirs[@]}"; do
    cp "${dir}/merge-${version}${dir}.profdata" "./plots/"
done

