## Store buffering experiments

While learning about memory consistency model, I want to observe the non sequential consistency (SC) behavior on x86 total store ordering (TSO). Normally, we can write litmus test and use herd tool suite [herdtools7](https://github.com/herd/herdtools7) to do so. Here is the store buffering litmus test

```
X86 SB
"Fre PodWR Fre PodWR"
{ x=0; y=0; }
 P0          | P1          ;
 MOV [x],$1  | MOV [y],$1  ;
 MOV EAX,[y] | MOV EAX,[x] ;
locations [x;y;]
exists (0:EAX=0 /\ 1:EAX=0)
```

However, I want to play around and observe the non-SC behavior using my own code :)

I mimic the litmus test in 2 ways:
- Using 2 concurrent threads
- Using 2 KVM vCPUs


### 1. Using threads
- Actually this is how litmus test is run using herd tool suite. My implementation is inspired a lot from the generated C code of litmus test by herd tool suite
- The herd tool suite generated test is more robust and run faster than mine as I don't try to run tests concurrently
- Run 500000 tests with this approach I observe the non-SC behavior 9 times on my machine (the number varies a lot, my main goal is to observe non-SC behavior so I don't try to get the average number here)

### 2. Using KVM vCPUs
- Curious to run the tests on bare CPU without much OS intervention, I play with KVM API to create 2 vCPUs running in real mode
- This approach run tests much slower than the previous one maybe because I do too much switch between host and guest, but it requires fewer tests to observe non-SC behavior
- Run 5000 tests I observe the non-SC behavior 10 times

Happy learning!
