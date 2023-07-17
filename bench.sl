(def fact-sub (& (z curr)
					(if (= z 1) curr (fact-sub (-- z) (* curr z)))))

(def fact (& (x) (fact-sub x 1)))

; 100k
; 0.139s release
; 0.077s release
; 0.049s release
; 0.030s release after = and --
; 0.036s release after gc
(print (fact 100000))

; 1M
; 365ms release (gc)
; 353ms release
;(fact 1000000)

