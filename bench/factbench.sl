;(def (eq x y) (if (- x y) false true))
(def fact-sub (& (z curr)
	(if z 
		(fact-sub (-- z) (* curr z))
		curr 
	)
))

(def fact (& (x) (fact-sub x 1)))
; 100k
; pre-compile: 48ms
; 26ms 
; 24ms
; 15ms (after goto)
; 10.9ms (after const stack)
; 9.2ms (after mul instr)
; 8.8ms (after closures)
; 7.6ms
; 7.0ms

; 1M
; pre-compile: 453ms
; 262ms
; 244ms (removed bind)
; 240ms
; 173ms (added =)
; 150ms (after goto)
; 136ms (after eq instr)
; 107ms (after const stack)
; 90ms (after mul instr)
; 88ms (after closures)
; 73ms
; 68ms

; 10M
; 1493ms
; 1340ms (after eq instr)
; 1027ms (after const stack)
; 887ms (after mul instr)
; 862ms (after closures)
; 725ms
; 671ms
(fact 10000000)
;(fact 100000)
