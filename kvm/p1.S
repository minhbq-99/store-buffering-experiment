BITS 16

out 1, al
mov BYTE [0], 1
mov al, BYTE [1]
mov BYTE [3], al
mfence
hlt
