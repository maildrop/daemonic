#!/bin/sh 

if [ -f /tmp/daemonic.pid ] ; then
    kill -INT `cat /tmp/daemonic.pid` ;
fi
