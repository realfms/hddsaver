#!/bin/bash

rm -rf /usr/src/saver6775-1.0
mkdir /usr/src/saver6775-1.0
cp -R . /usr/src/saver6775-1.0/
cp dkms.conf /usr/src/saver6775-1.0/
dkms add -m saver6775/1.0
dkms install -m saver6775/1.0
