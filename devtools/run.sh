#!/usr/bin/bash

make && sudo rmmod hid-hyperx; sudo insmod build/hid-hyperx.ko && sudo dmesg -w


