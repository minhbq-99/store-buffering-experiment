BITS 16

out 1, al
mov BYTE [1], 1
mov al, BYTE [0]
mov BYTE [2], al
mfence
hlt
