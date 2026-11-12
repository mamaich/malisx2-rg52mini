#!/bin/bash
# Run a command on the console over ssh: dev.sh '<command>'
# RG52_HOST: the console's address (required). RG52_USER: the login (default ark).
# RG52_PASS: the password, handed to sshpass; without it ssh uses your keys.
: "${RG52_HOST:?set RG52_HOST to the address of the console}"
O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5 -o ServerAliveInterval=10 -o ServerAliveCountMax=3"
if [ -n "${RG52_PASS:-}" ]; then
  SSHPASS=$RG52_PASS exec sshpass -e ssh $O "${RG52_USER:-ark}@$RG52_HOST" "$@"
fi
exec ssh $O "${RG52_USER:-ark}@$RG52_HOST" "$@"
