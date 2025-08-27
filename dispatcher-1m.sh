#!/bin/bash 

version="6"

dir="1m-i-fb-a"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer insane fuzzbench all "${version}${dir}" 1000000"
cd ..
sleep 1

dir="1m-i-sf-a"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer insane stateful all "${version}${dir}" 1000000"
cd ..
sleep 1

dir="1m-i-fb-q"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer insane fuzzbench queue "${version}${dir}" 1000000"
cd ..
sleep 1

dir="1m-i-sf-q"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer insane stateful queue "${version}${dir}" 1000000"
cd ..
sleep 1

dir="1m-s-fb-a"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer sane fuzzbench all "${version}${dir}" 1000000"
cd ..
sleep 1

dir="1m-s-sf-a"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer sane stateful all "${version}${dir}" 1000000"
cd ..
sleep 1

dir="1m-s-sl-a"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer sane stateless all "${version}${dir}" 1000000"
cd ..
sleep 1

dir="1m-s-fb-q"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer sane fuzzbench queue "${version}${dir}" 1000000"
cd ..
sleep 1

dir="1m-s-sf-q"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer sane stateful queue "${version}${dir}" 1000000"
cd ..
sleep 1

dir="1m-s-sl-q"
cd "$dir" || { echo "Failed to cd to $dir"; exit 1; }
rm -rf *
screen -dmS "${version}${dir}" bash -c "../out/CovalyzerHost ../hb-shape-fuzzer sane stateless queue "${version}${dir}" 1000000"
cd ..
sleep 1

