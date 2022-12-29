(def fact-sub (& (z curr)
					(if (= z 1) curr (fact-sub (- z 1) (* curr z)))))

(def fact (& (x) (fact-sub x 1)))

; 0.139s release
; 0.077s release
; 0.049s release
; 0.033s release after =
(fact 100000)
