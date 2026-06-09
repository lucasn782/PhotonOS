#!/bin/bash

# Script para testar ping no PhotonOS
(
  sleep 2
  echo "ping 10.0.2.2"
  sleep 5
  echo "exit"
) | qemu-system-x86_64 \
  -drive file=build/photon.img,format=raw,index=0,media=disk \
  -netdev user,id=net0 \
  -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -object filter-dump,id=f1,netdev=net0,file=logs/net.pcap \
  -serial stdio \
  -m 256 \
  -nographic
