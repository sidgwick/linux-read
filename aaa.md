Linux 内核中有下面的代码, 请分析. 重点回答下面几个点

1. jae 是怎么调转的?
2. cmp 是怎么比较的


```asm
cmpl _NR_syscalls,%eax
jae bad_sys_call
```