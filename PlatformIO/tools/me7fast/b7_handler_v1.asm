; ============================================================================
; can2gauge clean-room ME7 B7 fast-log handler - v1 CONFIG-DRIVEN GATHER
; Written from the ABI in docs/me7_ramroutine_re.md 6g.1 (NOT mki's bytes). C166/ST10, tasking.
; UNTESTED author's best-effort - bench-iterate after the v0 marker proves the inject/hook/poll chain.
;
; ABI: entered with R12 = response buffer base; write the response at [R12+0..] (ambient DPP, like the stock
; handler); return the byte count in RL4; return via rets.
;
; Our config sits right after this code, inside our blob (page 0xE1 at 0x387xxx):
;   word  0      : N  (number of variables, 1..127, stored as a 16-bit word)
;   then N x 4   : {page16, off16}  - each variable's EXTP page and 14-bit offset, filled by the firmware
; For each entry we read a 2-byte word from page:off and append it to the response. Returns N*2 bytes.
; EXTP overrides the page for the next 1 instruction only, so we re-issue it before every far read.
; CONFIG_OFF and LOOP are literal addresses patched in after a first assembly pass (assembler has no labels).
; ============================================================================
mov [-r0],r6         ; save callee-saved regs we use
mov [-r0],r7
mov [-r0],r8
mov r5,r12           ; r5 = response write cursor (starts at buffer base)
mov r7,#0xe1         ; r7 = our config's page (blob lives in page 0xE1)
mov r6,#0x3b00       ; r6 = config offset within page 0xE1 (= physical 0x387B00, just after this 58-byte handler)
extp r7,#0x1
mov r8,[r6]          ; read count N (word) from config into the loop counter
add r6,#0x2
extp r7,#0x1         ; --- LOOP start [PATCH: LOOP] ---
mov r3,[r6]          ; r3 = this variable's page
add r6,#0x2
extp r7,#0x1
mov r2,[r6]          ; r2 = this variable's 14-bit offset
add r6,#0x2
extp r3,#0x1
mov r4,[r2]          ; r4 = the variable value (word) at page:off
mov [r5],r4          ; write it to the response buffer (ambient DPP)
add r5,#0x2
sub r8,#0x1          ; N--
jmpr cc_NE,0x387adc  ; loop while N != 0  (0x387ADC = the 'extp r7' at the top of the loop body)
sub r5,r12           ; r5 = total bytes written = cursor - base
mov r4,r5            ; return length in RL4 (low byte)
mov r8,[r0+]         ; restore callee-saved
mov r7,[r0+]
mov r6,[r0+]
rets
