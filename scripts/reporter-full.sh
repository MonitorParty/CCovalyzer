#!/bin/bash 

version="10"

dir="i-fb-a"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="i-sf-a"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="i-fb-q"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="i-sf-q"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="s-fb-a"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="s-sf-a"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

#dir="s-sl-a"
#cd "$dir"
#llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
#llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
#cd ..

dir="s-fb-q"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="s-sf-q"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..

dir="s-sl-q"
cd "$dir"
llvm-profdata merge --sparse cov-9526650416236694333_0.profraw -o "merge-${version}${dir}.profdata"
llvm-cov report ../hb-shape-fuzzer --instr-profile="merge-${version}${dir}.profdata" > report.txt
cd ..


