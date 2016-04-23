#!/bin/sh 
if [ -f /tmp/daemonlize.pid ] ; then
    kill -INT `cat /tmp/daemonlize.pid` ;
fi
