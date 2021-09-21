#!/bin/bash

NNODES=2
NPROC_PER_NODE=1
MASTER_IP=10.28.1.16
MASTER_PORT=1234

export NCCL_SOCKET_IFNAME=eth2

python3 -m torch.distributed.run \
	--nnodes=$NNODES --nproc_per_node=$NPROC_PER_NODE \
	--rdzv_id=1234 --rdzv_backend=c10d \
	--rdzv_endpoint=$MASTER_IP \
	main.py \
	--master_ip $MASTER_IP \
	--master_port $MASTER_PORT \
