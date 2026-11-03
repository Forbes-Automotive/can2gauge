; ============================================================================
; can2gauge clean-room ME7 B7 fast-log handler - v0 SMOKE-TEST skeleton
; Written from the behavioural spec + ECU ABI in docs/me7_ramroutine_re.md 6g/6g.1
; (NOT from mki's blob - clean-room). Infineon C166/ST10, tasking syntax.
;
; ABI (pinned by RE): the ECU dispatcher tail-calls us with
;   R12 = response buffer base (XRAM: 0xE0A6 on 022, 0xE0A8 on 8N)
;   we write the response at [R12+0], [R12+1], ...  (the dispatcher already
;   consumed the request, so we read nothing from it)
;   we return the response byte-count in RL4 and return via  rets
;   (the ECU's own B7 service wraps our bytes into the KWP2000 reply)
;
; v0 just writes a fixed marker "C2G!" so a bench  B7  proves the whole
; install -> hook 0xE226 -> 3E test -> poll chain works end to end, before we
; trust the real value-gather (v1). Uses only scratch regs r4/r5 so nothing
; callee-saved is touched. Branch-free, so the per-line assembler is happy.
; ============================================================================
mov r5,r12          ; r5 = write cursor, starts at the buffer base
movb RL4,#0x43      ; 'C'
movb [r5],RL4       ; buffer[0]
add r5,#0x1
movb RL4,#0x32      ; '2'
movb [r5],RL4       ; buffer[1]
add r5,#0x1
movb RL4,#0x47      ; 'G'
movb [r5],RL4       ; buffer[2]
add r5,#0x1
movb RL4,#0x21      ; '!'
movb [r5],RL4       ; buffer[3]
movb RL4,#0x4       ; return value: response length = 4 bytes
rets                ; back to the ECU's B7 service, which frames the reply
