(def fact-sub (& (z curr)
					(if (= z 1) curr (fact-sub (-- z) (* curr z)))))

(def fact (& (x) (fact-sub x 1)))

; 0.139s release
; 0.077s release
; 0.049s release
; 0.030s release after = and --
(fact 100000)
