#!/bin/bash
TAP_DEV="xv6tap0"
HOST_IP="10.0.2.2/24"

if ! ip link show "$TAP_DEV" > /dev/null 2>&1; then
    echo "Creating TAP device $TAP_DEV..."
    sudo ip tuntap add dev "$TAP_DEV" mode tap
    sudo ip addr add "$HOST_IP" dev "$TAP_DEV"
    sudo ip link set dev "$TAP_DEV" up
    echo "$TAP_DEV is ready."
else
    echo "$TAP_DEV already exists."
fi