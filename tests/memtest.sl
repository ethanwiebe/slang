(import (slang gc))

(def make-int-list 
	(& (len) 
		(letrec ((recur-int-list 
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

(def testVal (letrec ((testloop 
	(& (x y)
		(if x
			(testloop (-- x) (++ y))
			y
		)
	)))
	testloop
))
(assert (= 7 (testVal 3 4)))

(def (loop a b) (+ a b))

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

(def N 100000)
(def makebig (& () (make-int-list N)))
(def biglist (make-int-list N))
(assert (= N (list-len biglist)))
(set-L! biglist 3)
(assert (= (list-len (makebig)) N))
(assert (= (len (makebig)) N))
(make-int-list 1000)
(assert (= (list-sum biglist) 4999950003))

(assert (= (gc-rec-size '(1 2 3)) 120))
(assert (= (gc-size '#(1 2 3)) 64))

(def bigvec 
	(vec-alloc N 0)
)
(assert (= (len bigvec) N))

(set! biglist ())
(set! list-sum ())
(set! make-int-list ())
(set! list-len ())
(set! makebig ())
(set! bigvec ())

(gc-collect)
; assert memory was cleaned up
(assert (< (gc-mem-cap) 100000))
(output "mem passed\n")
