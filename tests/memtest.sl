(def make-int-list 
	(& (len) 
		(let ((recur-int-list 
				(& (len list) 
					(if (= len 0) 
						list
						(recur-int-list (-- len) (pair (-- len) list))
					)
				))
			)
			(recur-int-list len ())
		)
	)
)
		
(def list-len (& (l)
	(let loop ((currLen 0) (list l))
		(if (= list ())
		currLen
		(loop (++ currLen) (R list))
		)
	)
))

(def list-sum (& (l)
	(let loop ((curr 0) (list l))
		(if (= list ())
		curr
		(loop (+ curr (L list)) (R list))
		)
	)
))

(def makebig (& () (make-int-list 100000)))
(def biglist (make-int-list 100000))
(assert (= 100000 (list-len biglist)))
(setL! biglist 3)
(assert (= (list-len (makebig)) 100000))
(make-int-list 1000)
(assert (= (list-sum biglist) 4999950003))
(set! biglist ())
(gc-collect)
(print 'passed)
