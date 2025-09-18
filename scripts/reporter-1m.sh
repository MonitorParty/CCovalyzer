#!/bin/bash 

version="10"

dir="1m-i-fb-a"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="1m-i-sf-a"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="1m-i-fb-q"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="1m-i-sf-q"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="1m-s-fb-a"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="1m-s-sf-a"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="1m-s-sl-a"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="1m-s-fb-q"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="1m-s-sf-q"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="1m-s-sl-q"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..


