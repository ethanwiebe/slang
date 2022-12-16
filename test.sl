(+ 12 (+ 1 1))
(def abc 13)
(+ abc 1)
12
(def func 
	(lambda (y) (do 
					(def x 1)
					(+ x y))))





(def eq (lambda (x y)
				(if (- x y) false true)))

(def fact-sub (lambda (z curr)
					(if (eq z 1) curr (fact-sub (- z 1) (* curr z)))))

(def fact (lambda (x) (fact-sub x 1)))

(+ 1 2)

(def nest 
	(lambda (x) 
		(lambda (y)
			(* x y)
		)
	)
)

(def counter
	(lambda (x)
		(do
			(def countVal x)
			(lambda () 
				(set! countVal (+ countVal 1))
			)
		)
	)
)

(def mul-by-3 (nest 3))
(mul-by-3 5)

(def c1 (counter 3))
(def c2 (counter 8))

(def test (lambda (x) (if (eq x 0) 0 (test (- x 1)))))

(c1)
(c1)
