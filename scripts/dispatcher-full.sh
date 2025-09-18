#!/bin/bash 

version="14n"

dir="i-fb-a"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer insane fuzzbench all "${version}${dir}" 22400000 1598"
cd ..
sleep 1

dir="i-sf-a"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer insane stateful all "${version}${dir}" 22400000 1598"
cd ..
sleep 1

dir="i-fb-q"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer insane fuzzbench queue "${version}${dir}" 22400000 1598"
cd ..
sleep 1

dir="i-sf-q"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer insane stateful queue "${version}${dir}" 22400000 1598"
cd ..
sleep 1

dir="s-fb-a"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer sane fuzzbench all "${version}${dir}" 22400000 1598"
cd ..
sleep 1

dir="s-sf-a"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer sane stateful all "${version}${dir}" 22400000 1598"
cd ..
sleep 1

dir="s-sl-a"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer sane stateless all "${version}${dir}" 22400000 1598"
cd ..
sleep 1

dir="s-fb-q"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer sane fuzzbench queue "${version}${dir}" 22400000 1598"
cd ..
sleep 1

dir="s-sf-q"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer sane stateful queue "${version}${dir}" 22400000 1598"
cd ..
sleep 1

dir="s-sl-q"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer sane stateless queue "${version}${dir}" 22400000 1598"
cd ..
sleep 1

