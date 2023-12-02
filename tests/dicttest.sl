; dict test

(def (assert-eq x y)
	(if (= x y)
		true
		(do
			(print x '!= y)
			(assert false)
		)
	)
)

(def d (dict))
(assert-eq 0 (len d))
(dict-set! d 'a 13)
(dict-set! d 'b 10)
(assert-eq 13 (dict-pop! d 'a))
(dict-set! d 3 -1)
(dict-set! d () "null")
(assert-eq 3 (len d))
(assert-eq -1 (dict-get d 3))
(assert-eq "null" (dict-get d ()))
(assert-eq "not found" (dict-get d 'c "not found"))
(assert (empty? (try (dict-get d 'e))))
(assert-eq -1 (dict-pop! d 3))
(assert-eq 2 (len d))
(assert-eq 10 (dict-pop! d 'b))
(assert-eq "null" (dict-pop! d ()))
(assert-eq 0 (len d))
(assert (empty? d))

(output "dict passed\n")
