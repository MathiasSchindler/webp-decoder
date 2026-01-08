.global _start
.type _start, @function
_start:
	xor %rbp, %rbp
	mov (%rsp), %rdi          # argc
	lea 8(%rsp), %rsi         # argv
	# envp would be at: 16(%rsp,%rdi,8)
	call main
	mov %eax, %edi            # exit code
	mov $60, %eax             # __NR_exit
	syscall
