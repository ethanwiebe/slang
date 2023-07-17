(print 'hi)
(+ 12 (+ 1 1))
(def abc 13)
(+ abc 1)
12
(def func 
	(& (y) 
				(def x 1)
				(+ x y)))



(def eq (& (x y)
				(if (- x y) false true)
				))

(def fact-sub (& (z curr)
	(if (eq z 1)
		curr 
		(fact-sub (-- z) (* curr z))
	)
))

(def fact (& (x) (fact-sub x 1)))

(def interm1 (& (x y) (+ x y)))
(def interm2 (& (x y) (* x y)))


(def combo (& (a b)
	(* (interm1 a b) (interm2 a b))
))

(assert (= (combo 2 4) 48))
(print (combo (+ 2 4) (+ 4 6)))
(print (combo (combo 2 4) 1))
(assert (= (combo (combo 2 4) 17) 53040))

(def fact-linear (& (x)
	(print x)
	(if
		(= x 1)
		1
		(* x (fact-linear (- x 1)))
	)
))

(print (fact-linear 8))
;(print 'start)
(print (fact 50))
;(print 'end)


(def nest 
	(& (x) 
		(& (y)
			(print x)
			(print y)
			(* x y)
		)
	)
)

(def counter
	(& (x)
		(print x)
		(& () 
			(set! x (+ x 1))
		)
	)
)

(def mul-by-3 (nest 3))
(print (mul-by-3 5))

(print 'c1)
(def c1 (counter 3))
(def c2 (counter 8))

(def test 
(& (x) (if (eq x 0) 0 (test (- x 1))))
)

(def a (/ 9 2))

(c1)
(c1)
(print (c1))
(print (c1))
(print (c2))

(print (= ''(1 (1 1.0)) ''(1 (1 1))))
(print (/ 2 -2))
(print (% 1 -10))
(assert (= '() ()))

(def x 10)
(print x)
(print (is '(1) ()))
(print (is (R '(1)) ()))

(def print-list (& (l)
	(print (L l))
	(if (is (R l) ()) 
		1 
		(print-list (R l))
	)
))

(print-list '(1 2 3 hello))

(def vary (& args
	(print-list args)
))

(vary 1 2 3 4 vary)

(print ((if true + *) 3 4))

(print ((& (x) (+ x 17)) 23))

(def lettest
	(let 
		((x 0)
		(y 1))
		(+ x y)
	)
)

(print lettest)

;(def letcount
	(let
		((count 0))
		(& ()
			(set! count (++ count))
			count
		)
	)
;)

;(print (letcount))
;(print (letcount))
;(print (letcount))

(def make-let-counter
	(& ()
		(let ((count 0))
			(& (count)
				(set! count (++ count))
				count
			)
		)
	)
)

(make-let-counter)
(print 3)
