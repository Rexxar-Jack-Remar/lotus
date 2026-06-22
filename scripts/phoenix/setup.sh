#!/bin/bash

export PATH=$PATH:/root/lotus/build/bin

if [ ! -d /root/work ]; then
    mkdir /root/work
    mkdir /root/work/config
    cp /root/lotus/config/*.spec /root/work/config
fi
