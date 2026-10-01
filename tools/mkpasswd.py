#!/usr/bin/env python3
# Prints an /etc/shadow password field in the format the Aegis kernel checks:
#   $aegis-sha256$ITERATIONS$SALT_HEX$HASH_HEX
import hashlib
import os
import sys

ITERATIONS = 10000

password = sys.argv[1].encode() if len(sys.argv) > 1 else sys.stdin.readline().rstrip("\n").encode()
salt = os.urandom(16)
h = hashlib.sha256(salt + password).digest()
for _ in range(ITERATIONS - 1):
    h = hashlib.sha256(h + salt + password).digest()
print(f"$aegis-sha256${ITERATIONS}${salt.hex()}${h.hex()}")
