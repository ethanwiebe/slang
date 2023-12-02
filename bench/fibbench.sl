(def (fib-sub a b i) 
	(if i 
		(fib-sub b (+ a b) (-- i))
		a 
	)
)

(def (fib n) (fib-sub 0.0 1.0 n))

; 5.006s
; 4.551s
; 0.961s
; 0.923s
; 0.853s
; 786ms
(fib 10000000)
